#ifndef SCREEN_UI_SCHEDULE_H
#define SCREEN_UI_SCHEDULE_H

#include "freertos/FreeRTOS.h"
#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace SCREEN {

/** Deadline arithmetic assumes each interval is less than half the tick range. */
class UiSchedule {
  public:
    // Reserve at least 25% of each active-work/recovery cycle for lower-priority
    // tasks. This is a CPU budget, independent of each page's target frame rate.
    static constexpr uint32_t MAX_WORK_SHARE_PERCENT = 75;
    static_assert(MAX_WORK_SHARE_PERCENT > 0 && MAX_WORK_SHARE_PERCENT < 100);

    static bool due(TickType_t now, TickType_t deadline) {
        return static_cast<std::make_signed_t<TickType_t>>(now - deadline) >= 0;
    }
    static TickType_t remaining(TickType_t now, TickType_t deadline) {
        return due(now, deadline) ? 0 : deadline - now;
    }
    static TickType_t interval(uint32_t ms) {
        return std::max<TickType_t>(1, pdMS_TO_TICKS(ms));
    }
    static TickType_t next_frame(TickType_t started, TickType_t finished, TickType_t period) {
        // Drop missed slots; never queue catch-up renders after a slow frame.
        return started + ((finished - started) / period + 1) * period;
    }
    static TickType_t recovery(TickType_t work_ticks) {
        return static_cast<TickType_t>((static_cast<uint64_t>(work_ticks) *
            (100 - MAX_WORK_SHARE_PERCENT) + MAX_WORK_SHARE_PERCENT - 1) / MAX_WORK_SHARE_PERCENT);
    }
};

} // namespace SCREEN
#endif
