#pragma once

#include <chrono>
#include <cstdint>

namespace moq2ts {

// Microseconds on the monotonic steady clock. Use for deltas only (the absolute
// value is arbitrary). The capture epoch and the pacing start are both readings
// of this clock, so an object's media time (epoch-relative) and the elapsed time
// (start-relative) are comparable.
inline std::int64_t nowSteadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// How long (microseconds) to wait before releasing an object whose media time is
// objectMediaUs, given elapsedUs since the first object and a latency slack.
// Returns 0 when the object is due or overdue (never adds catch-up delay).
inline std::int64_t paceDelayUs(std::int64_t objectMediaUs, std::int64_t elapsedUs,
                                std::int64_t slackUs) {
    const std::int64_t delay = objectMediaUs - slackUs - elapsedUs;
    return delay > 0 ? delay : 0;
}

}  // namespace moq2ts
