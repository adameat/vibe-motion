#include "vibe_motion/event.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace vibe_motion;
using namespace std::chrono_literals;

int main() {
    EventStateMachine state({3, 30s, 2});
    const auto start = std::chrono::steady_clock::time_point{};
    assert(!state.update(true, start).event_started);
    assert(!state.update(true, start + 1s).event_started);
    auto decision = state.update(true, start + 2s);
    assert(decision.event_started && decision.motion_detected && decision.event_number == 1);
    assert(state.update(false, start + 3s).record_frame);
    assert(state.update(false, start + 4s).record_frame);
    assert(!state.update(false, start + 5s).record_frame);
    assert(!state.update(false, start + 31s).event_ended);
    decision = state.update(false, start + 32s);
    assert(decision.event_ended && !state.active());

    state.update(true, start + 40s);
    state.update(true, start + 41s);
    decision = state.update(true, start + 42s);
    assert(decision.event_started && decision.event_number == 2);
    assert(state.stop().event_ended);
    assert(!state.stop().event_ended);

    std::cout << "event tests passed\n";
}
