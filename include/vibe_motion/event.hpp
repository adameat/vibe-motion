#pragma once

#include "vibe_motion/detection.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace vibe_motion {

struct EventSettings {
    int minimum_motion_frames = 1;
    std::chrono::seconds event_gap{60};
    int post_capture_frames = 0;
};

struct EventDecision {
    bool motion_detected = false;
    bool event_started = false;
    bool event_ended = false;
    bool record_frame = false;
    std::uint64_t event_number = 0;
};

class EventStateMachine {
  public:
    explicit EventStateMachine(EventSettings settings);
    EventDecision update(bool qualifying_motion, std::chrono::steady_clock::time_point now);
    EventDecision stop();
    bool active() const noexcept {
        return active_;
    }
    std::uint64_t event_number() const noexcept {
        return event_number_;
    }

  private:
    EventSettings settings_;
    bool active_ = false;
    int consecutive_motion_ = 0;
    int post_capture_remaining_ = 0;
    std::uint64_t event_number_ = 0;
    std::optional<std::chrono::steady_clock::time_point> last_motion_;
};

} // namespace vibe_motion
