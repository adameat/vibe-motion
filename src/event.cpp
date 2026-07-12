#include "vibe_motion/event.hpp"

#include <algorithm>
#include <stdexcept>

namespace vibe_motion {

EventStateMachine::EventStateMachine(EventSettings settings) : settings_(settings) {
    if (settings_.minimum_motion_frames < 1 || settings_.post_capture_frames < 0 ||
        settings_.event_gap.count() < 0) {
        throw std::invalid_argument("invalid event state settings");
    }
}

EventDecision EventStateMachine::update(bool qualifying_motion,
                                        std::chrono::steady_clock::time_point now) {
    EventDecision decision;
    if (qualifying_motion) {
        consecutive_motion_ = std::min(consecutive_motion_ + 1, settings_.minimum_motion_frames);
    } else {
        consecutive_motion_ = 0;
    }

    const bool confirmed =
        qualifying_motion && consecutive_motion_ >= settings_.minimum_motion_frames;
    if (confirmed) {
        decision.motion_detected = true;
        last_motion_ = now;
        post_capture_remaining_ = settings_.post_capture_frames;
        if (!active_) {
            active_ = true;
            ++event_number_;
            decision.event_started = true;
        }
    } else if (active_ && post_capture_remaining_ > 0) {
        decision.record_frame = true;
        --post_capture_remaining_;
    }

    decision.record_frame = decision.record_frame || (active_ && confirmed);
    if (active_ && last_motion_ && !qualifying_motion &&
        now - *last_motion_ >= settings_.event_gap) {
        active_ = false;
        decision.event_ended = true;
        decision.record_frame = false;
        last_motion_.reset();
        post_capture_remaining_ = 0;
    }
    decision.event_number = event_number_;
    return decision;
}

EventDecision EventStateMachine::stop() {
    EventDecision decision;
    decision.event_number = event_number_;
    if (active_) {
        active_ = false;
        decision.event_ended = true;
    }
    last_motion_.reset();
    post_capture_remaining_ = 0;
    consecutive_motion_ = 0;
    return decision;
}

} // namespace vibe_motion
