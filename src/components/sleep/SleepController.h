#pragma once

#include <array>
#include <cstdint>
#include "components/settings/Settings.h"

namespace Pinetime {
  namespace Controllers {
    class DateTime;
    class FS;
    class HeartRateController;

    // Records a night: for each minute of a session, how much the wrist moved
    // and the heart rate if a good reading was current. The night is kept in
    // RAM and written to /sleep/<start>.slp by Flush(), which only the display
    // task calls (the filesystem has no locking): whenever the screen comes on,
    // and when the session stops. So nothing wakes the watch at night.
    //
    // File format (little endian), for tools reading it off the watch:
    //   FileHeader, then `minutes` × Minute, one per minute from `startUtc`.
    class SleepController {
    public:
      struct Minute {
        // Sum of |Δx|+|Δy|+|Δz| between the 10 Hz motion samples, / 4, capped.
        uint16_t movement;
        // Beats per minute, 0 when no good reading was current.
        uint8_t heartRate;
        uint8_t flags;
      };

      static_assert(sizeof(Minute) == 4);

      // The watch wasn't recording this minute (it restarted mid-session).
      static constexpr uint8_t FlagNoData = 1;
      static constexpr uint16_t maxMinutes = 12 * 60;
      static constexpr uint8_t keepNights = 14;

      struct FileHeader {
        char magic[4]; // "SLP1"
        uint32_t startUtc;
        int16_t tzOffsetMinutes;
        uint16_t minutes;
      };

      static_assert(sizeof(FileHeader) == 12);

      SleepController(FS& fs, DateTime& dateTime, HeartRateController& heartRate, Settings& settings);

      // System task, at boot: resumes a session that was running.
      void Init();

      // Display task (the app's buttons).
      void Start();
      void Stop();
      void Flush();

      // System task, every motion sample (10 Hz).
      void OnMotion(int16_t x, int16_t y, int16_t z);

      bool IsTracking() const {
        return tracking;
      }

      uint32_t StartUtc() const {
        return startUtc;
      }

      uint16_t MinutesRecorded() const {
        return count;
      }

      // The last finished minute, or nullptr before the first.
      const Minute* LastMinute() const {
        return count == 0 ? nullptr : &minutes[count - 1];
      }

    private:
      struct State {
        uint8_t version;
        bool tracking;
        uint32_t startUtc;
        // What the session changed, to put back when it stops.
        Settings::Notification notification;
        bool heartRateWasEnabled;
        bool heartRateIntervalWasSet;
        uint16_t heartRateInterval;
      };

      static constexpr uint8_t stateVersion = 1;
      static constexpr uint16_t heartRateIntervalSeconds = 5 * 60;

      uint32_t NowMinute();
      void CloseMinute(uint32_t nowMinute);
      void LoadState();
      void SaveState();
      void LoadNight();
      void PruneOldNights();
      void NightPath(char* buffer, size_t size) const;

      FS& fs;
      DateTime& dateTime;
      HeartRateController& heartRate;
      Settings& settings;

      State state {};
      volatile bool tracking = false;
      uint32_t startUtc = 0;
      int16_t tzOffsetMinutes = 0;

      std::array<Minute, maxMinutes> minutes {};
      volatile uint16_t count = 0;
      bool dirty = false;

      uint32_t currentMinute = 0;
      uint32_t movement = 0;
      bool hasPrevious = false;
      int16_t px = 0, py = 0, pz = 0;
    };
  }
}
