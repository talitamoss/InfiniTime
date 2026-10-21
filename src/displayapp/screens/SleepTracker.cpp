#include "displayapp/screens/SleepTracker.h"

#include <chrono>
#include "components/datetime/DateTimeController.h"
#include "displayapp/DisplayApp.h"
#include "displayapp/InfiniTimeTheme.h"

using namespace Pinetime::Applications::Screens;

namespace {
  void ButtonEventHandler(lv_obj_t* obj, lv_event_t event) {
    static_cast<SleepTracker*>(obj->user_data)->OnButton(event);
  }
}

SleepTracker::SleepTracker(Controllers::SleepController& sleep, Controllers::DateTime& dateTime) : sleep {sleep}, dateTime {dateTime} {
  title = lv_label_create(lv_scr_act(), nullptr);
  lv_label_set_text_static(title, "Sleep");
  lv_obj_set_style_local_text_color(title, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, Colors::lightGray);
  lv_obj_align(title, nullptr, LV_ALIGN_IN_TOP_MID, 0, 8);

  status = lv_label_create(lv_scr_act(), nullptr);
  lv_obj_set_style_local_text_font(status, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, &jetbrains_mono_42);

  detail = lv_label_create(lv_scr_act(), nullptr);
  lv_label_set_long_mode(detail, LV_LABEL_LONG_BREAK);
  lv_obj_set_width(detail, LV_HOR_RES - 20);
  lv_label_set_align(detail, LV_LABEL_ALIGN_CENTER);
  lv_obj_set_style_local_text_color(detail, LV_LABEL_PART_MAIN, LV_STATE_DEFAULT, Colors::lightGray);

  button = lv_btn_create(lv_scr_act(), nullptr);
  button->user_data = this;
  lv_obj_set_event_cb(button, ButtonEventHandler);
  lv_obj_set_size(button, LV_HOR_RES - 20, 64);
  lv_obj_align(button, nullptr, LV_ALIGN_IN_BOTTOM_MID, 0, -6);
  buttonLabel = lv_label_create(button, nullptr);

  // The screen coming on is when the night so far gets saved.
  sleep.Flush();
  Show();
  taskRefresh = lv_task_create(RefreshTaskCallback, 1000, LV_TASK_PRIO_MID, this);
}

SleepTracker::~SleepTracker() {
  lv_task_del(taskRefresh);
  lv_obj_clean(lv_scr_act());
}

void SleepTracker::OnButton(lv_event_t event) {
  if (!sleep.IsTracking() && event == LV_EVENT_CLICKED) {
    sleep.Start();
    Show();
  } else if (sleep.IsTracking() && event == LV_EVENT_LONG_PRESSED) {
    sleep.Stop();
    Show();
  }
}

void SleepTracker::Show() {
  shownTracking = sleep.IsTracking();
  if (shownTracking) {
    lv_label_set_text_static(buttonLabel, "Hold to stop");
    lv_obj_set_style_local_bg_color(button, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, Colors::bgAlt);
  } else {
    lv_label_set_text_static(buttonLabel, "Start");
    lv_obj_set_style_local_bg_color(button, LV_BTN_PART_MAIN, LV_STATE_DEFAULT, Colors::highlight);
  }
  Refresh();
}

void SleepTracker::Refresh() {
  if (sleep.IsTracking() != shownTracking) {
    Show();
    return;
  }

  if (!sleep.IsTracking()) {
    lv_label_set_text_static(status, "--:--");
    if (sleep.MinutesRecorded() > 0) {
      lv_label_set_text_fmt(detail, "Last night: %dh %02dm saved", sleep.MinutesRecorded() / 60, sleep.MinutesRecorded() % 60);
    } else {
      lv_label_set_text_static(detail, "Quiets the watch and records the night");
    }
  } else {
    auto utc = dateTime.UTCDateTime();
    auto nowUtc = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(utc.time_since_epoch()).count());
    uint32_t elapsed = nowUtc > sleep.StartUtc() ? (nowUtc - sleep.StartUtc()) / 60 : 0;
    lv_label_set_text_fmt(status, "%lu:%02lu", static_cast<unsigned long>(elapsed / 60), static_cast<unsigned long>(elapsed % 60));

    const auto* last = sleep.LastMinute();
    if (last == nullptr) {
      lv_label_set_text_static(detail, "Recording. Sleep well.");
    } else if (last->heartRate != 0) {
      lv_label_set_text_fmt(detail, "Moved %u  |  %u bpm", last->movement, last->heartRate);
    } else {
      lv_label_set_text_fmt(detail, "Moved %u  |  no pulse yet", last->movement);
    }
  }
  lv_obj_align(status, nullptr, LV_ALIGN_CENTER, 0, -30);
  lv_obj_align(detail, status, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
}
