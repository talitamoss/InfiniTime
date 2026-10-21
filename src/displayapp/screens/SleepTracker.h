#pragma once

#include <lvgl/lvgl.h>
#include "displayapp/apps/Apps.h"
#include "displayapp/Controllers.h"
#include "displayapp/screens/Screen.h"
#include "displayapp/screens/Symbols.h"
#include "components/sleep/SleepController.h"

namespace Pinetime {
  namespace Controllers {
    class DateTime;
  }

  namespace Applications {
    namespace Screens {

      // Start a night, or see and stop the one running. Stopping takes a long
      // press, so a tap in the dark doesn't end it.
      class SleepTracker : public Screen {
      public:
        SleepTracker(Controllers::SleepController& sleep, Controllers::DateTime& dateTime);
        ~SleepTracker() override;

        void Refresh() override;
        void OnButton(lv_event_t event);

      private:
        void Show();

        Controllers::SleepController& sleep;
        Controllers::DateTime& dateTime;

        lv_obj_t* title;
        lv_obj_t* status;
        lv_obj_t* detail;
        lv_obj_t* button;
        lv_obj_t* buttonLabel;
        lv_task_t* taskRefresh;
        bool shownTracking = false;
      };
    }
  }

  namespace Applications {
    template <>
    struct AppTraits<Apps::SleepTracker> {
      static constexpr Apps app = Apps::SleepTracker;
      static constexpr const char* icon = Screens::Symbols::sleep;

      static Screens::Screen* Create(AppControllers& controllers) {
        return new Screens::SleepTracker(controllers.sleepController, controllers.dateTimeController);
      }

      static bool IsAvailable(Pinetime::Controllers::FS& /*filesystem*/) {
        return true;
      }
    };
  }
}
