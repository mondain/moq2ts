#include "media/EgressPacing.h"

#include <iostream>
#include <string>

using moq2ts::paceDelayUs;

namespace {
bool expect(bool cond, const std::string& msg) {
    if (!cond) { std::cerr << "FAIL: " << msg << '\n'; return false; }
    return true;
}
}  // namespace

int main() {
    bool ok = true;

    // Ahead of clock: wait ~ mediaUs - slack - elapsed.
    ok &= expect(paceDelayUs(100000, 0, 8000) == 92000, "ahead -> positive delay");

    // Within slack: due now.
    ok &= expect(paceDelayUs(5000, 0, 8000) == 0, "within slack -> 0");

    // Exactly at media time minus slack boundary.
    ok &= expect(paceDelayUs(8000, 0, 8000) == 0, "boundary -> 0");

    // Overdue (elapsed past media time): no catch-up delay.
    ok &= expect(paceDelayUs(50000, 200000, 8000) == 0, "overdue -> 0");

    // Negative media time (garbage/clock-backward): never sleeps.
    ok &= expect(paceDelayUs(-1000, 0, 8000) == 0, "negative media -> 0");

    // Steady-state: object 1s out, 0.9s elapsed, 8ms slack -> ~92ms wait.
    ok &= expect(paceDelayUs(1000000, 900000, 8000) == 92000, "steady-state delay");

    return ok ? 0 : 1;
}
