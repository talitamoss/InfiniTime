#include "components/sleep/SleepController.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <FreeRTOS.h>
#include <task.h>
#include <libraries/log/nrf_log.h>
#include "components/datetime/DateTimeController.h"
#include "components/fs/FS.h"
#include "components/heartrate/HeartRateController.h"

using namespace Pinetime::Controllers;

namespace {
  constexpr const char* stateFile = "/.system/sleep.dat";
  constexpr const char* nightDir = "/sleep";
  constexpr char magic[4] = {'S', 'L', 'P', '1'};
}

SleepController::SleepController(FS& fs, DateTime& dateTime, HeartRateController& heartRate, Settings& settings)
  : fs {fs}, dateTime {dateTime}, heartRate {heartRate}, settings {settings} {
}

uint32_t SleepController::NowMinute() {
  auto utc = dateTime.UTCDateTime();
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::minutes>(utc.time_since_epoch()).count());
}

void SleepController::Init() {
  LoadState();
  if (!state.tracking) {
    return;
  }
  startUtc = state.startUtc;
  tzOffsetMinutes = static_cast<int16_t>(dateTime.UtcOffset() * 15);
  LoadNight();
  // Minutes from the restart until now are filled in as "no data" by the
  // first OnMotion.
  currentMinute = startUtc / 60 + count;
  movement = 0;
  hasPrevious = false;
  tracking = true;
  NRF_LOG_INFO("[sleep] resumed a session with %d minutes", count);
}

void SleepController::Start() {
  if (tracking) {
    return;
  }
  PruneOldNights();

  auto now = NowMinute();
  startUtc = now * 60;
  tzOffsetMinutes = static_cast<int16_t>(dateTime.UtcOffset() * 15);

  // Quiet the watch for the night, and measure heart rate in the background.
  // Both are put back by Stop().
  state.version = stateVersion;
  state.tracking = true;
  state.startUtc = startUtc;
  state.notification = settings.GetNotificationStatus();
  state.heartRateWasEnabled = heartRate.State() != HeartRateController::States::Disabled;
  auto interval = settings.GetHeartRateBackgroundMeasurementInterval();
  state.heartRateIntervalWasSet = interval.has_value();
  state.heartRateInterval = interval.value_or(0);

  settings.SetNotificationStatus(Settings::Notification::Sleep);
  if (!interval.has_value() || interval.value() > heartRateIntervalSeconds) {
    settings.SetHeartRateBackgroundMeasurementInterval(heartRateIntervalSeconds);
  }
  settings.SaveSettings();
  if (!state.heartRateWasEnabled) {
    heartRate.Enable();
  }

  taskENTER_CRITICAL();
  count = 0;
  currentMinute = now;
  movement = 0;
  hasPrevious = false;
  tracking = true;
  taskEXIT_CRITICAL();

  dirty = true;
  SaveState();
  Flush();
  NRF_LOG_INFO("[sleep] started");
}

void SleepController::Stop() {
  if (!tracking) {
    return;
  }
  auto now = NowMinute();
  taskENTER_CRITICAL();
  tracking = false;
  CloseMinute(now + 1);
  taskEXIT_CRITICAL();
  Flush();

  settings.SetNotificationStatus(state.notification);
  settings.SetHeartRateBackgroundMeasurementInterval(state.heartRateIntervalWasSet ? std::optional<uint16_t>(state.heartRateInterval)
                                                                                    : std::nullopt);
  settings.SaveSettings();
  if (!state.heartRateWasEnabled) {
    heartRate.Disable();
  }

  state.tracking = false;
  SaveState();
  NRF_LOG_INFO("[sleep] stopped after %d minutes", count);
}

void SleepController::OnMotion(int16_t x, int16_t y, int16_t z) {
  if (!tracking) {
    return;
  }
  if (hasPrevious) {
    movement += static_cast<uint32_t>(std::abs(x - px) + std::abs(y - py) + std::abs(z - pz));
  }
  px = x;
  py = y;
  pz = z;
  hasPrevious = true;

  auto now = NowMinute();
  if (now != currentMinute) {
    // Stop() closes the last minute from the display task; don't race it.
    taskENTER_CRITICAL();
    if (tracking) {
      CloseMinute(now);
    }
    taskEXIT_CRITICAL();
  }
}

// Finishes the minute that was running and moves on to `nowMinute`. Minutes
// skipped in between (a restart) are recorded as having no data.
void SleepController::CloseMinute(uint32_t nowMinute) {
  if (nowMinute <= currentMinute) {
    return;
  }
  uint16_t n = count;
  if (n < maxMinutes) {
    Minute m {};
    m.movement = static_cast<uint16_t>(std::min<uint32_t>(movement / 4, UINT16_MAX));
    m.heartRate = heartRate.State() == HeartRateController::States::Measuring ? heartRate.HeartRate() : 0;
    minutes[n++] = m;
  }
  for (uint32_t gap = currentMinute + 1; gap < nowMinute && n < maxMinutes; gap++) {
    minutes[n++] = Minute {0, 0, FlagNoData};
  }
  // Records are written before the count moves on, so a reader that takes
  // the count first never sees a half-written minute.
  count = n;
  dirty = true;
  currentMinute = nowMinute;
  movement = 0;
}

void SleepController::NightPath(char* buffer, size_t size) const {
  // Fixed-width hex of the start, so names sort by date.
  snprintf(buffer, size, "%s/%08lx.slp", nightDir, static_cast<unsigned long>(startUtc));
}

void SleepController::Flush() {
  if (!dirty || startUtc == 0) {
    return;
  }
  lfs_dir_t dir;
  if (fs.DirOpen(nightDir, &dir) != LFS_ERR_OK) {
    fs.DirCreate(nightDir);
  } else {
    fs.DirClose(&dir);
  }

  char path[32];
  NightPath(path, sizeof(path));
  lfs_file_t file;
  if (fs.FileOpen(&file, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != LFS_ERR_OK) {
    NRF_LOG_WARNING("[sleep] could not open %s", path);
    return;
  }
  uint16_t n = count;
  FileHeader header {};
  std::memcpy(header.magic, magic, sizeof(magic));
  header.startUtc = startUtc;
  header.tzOffsetMinutes = tzOffsetMinutes;
  header.minutes = n;
  fs.FileWrite(&file, reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  fs.FileWrite(&file, reinterpret_cast<const uint8_t*>(minutes.data()), n * sizeof(Minute));
  fs.FileClose(&file);
  if (n == count) {
    dirty = false;
  }
}

void SleepController::LoadNight() {
  char path[32];
  NightPath(path, sizeof(path));
  lfs_file_t file;
  count = 0;
  if (fs.FileOpen(&file, path, LFS_O_RDONLY) != LFS_ERR_OK) {
    return;
  }
  FileHeader header {};
  if (fs.FileRead(&file, reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
      std::memcmp(header.magic, magic, sizeof(magic)) == 0 && header.startUtc == startUtc) {
    uint16_t n = std::min(header.minutes, maxMinutes);
    if (fs.FileRead(&file, reinterpret_cast<uint8_t*>(minutes.data()), n * sizeof(Minute)) == static_cast<int>(n * sizeof(Minute))) {
      count = n;
    }
  }
  fs.FileClose(&file);
}

// Keeps the newest keepNights - 1 nights, making room for the one starting.
// Names are fixed-width hex of the start, so the smallest is the oldest.
void SleepController::PruneOldNights() {
  constexpr size_t nameLength = 12; // 8 hex + ".slp"
  while (true) {
    lfs_dir_t dir;
    if (fs.DirOpen(nightDir, &dir) != LFS_ERR_OK) {
      return;
    }
    size_t nights = 0;
    char oldest[nameLength + 1] = {};
    lfs_info info;
    while (fs.DirRead(&dir, &info) > 0) {
      if (info.type != LFS_TYPE_REG || std::strlen(info.name) != nameLength) {
        continue;
      }
      if (nights == 0 || std::strcmp(info.name, oldest) < 0) {
        std::memcpy(oldest, info.name, nameLength + 1);
      }
      nights++;
    }
    fs.DirClose(&dir);
    if (nights < keepNights) {
      return;
    }
    char path[32];
    snprintf(path, sizeof(path), "%s/%s", nightDir, oldest);
    if (fs.FileDelete(path) != LFS_ERR_OK) {
      return;
    }
  }
}

void SleepController::LoadState() {
  lfs_file_t file;
  State buffer {};
  if (fs.FileOpen(&file, stateFile, LFS_O_RDONLY) != LFS_ERR_OK) {
    return;
  }
  auto read = fs.FileRead(&file, reinterpret_cast<uint8_t*>(&buffer), sizeof(buffer));
  fs.FileClose(&file);
  if (read == sizeof(buffer) && buffer.version == stateVersion) {
    state = buffer;
  }
}

void SleepController::SaveState() {
  lfs_file_t file;
  if (fs.FileOpen(&file, stateFile, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != LFS_ERR_OK) {
    NRF_LOG_WARNING("[sleep] could not save state");
    return;
  }
  fs.FileWrite(&file, reinterpret_cast<const uint8_t*>(&state), sizeof(state));
  fs.FileClose(&file);
}
