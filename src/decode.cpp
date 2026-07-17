#include "vibe_motion/decode.hpp"

#include <stdexcept>

namespace vibe_motion {

std::string_view frame_decode_mode_name(FrameDecodeMode mode) noexcept {
    switch (mode) {
    case FrameDecodeMode::all:
        return "all";
    case FrameDecodeMode::keyframes:
        return "keyframes";
    }
    return "unknown";
}

FrameDecodeController::FrameDecodeController(FrameDecodeMode idle_mode,
                                             std::chrono::milliseconds grace_period)
    : idle_mode_(idle_mode), active_mode_(idle_mode), requested_mode_(idle_mode),
      grace_period_(grace_period) {
    if (grace_period_.count() < 0) {
        throw std::invalid_argument("decode grace period cannot be negative");
    }
}

FrameDecodeMode FrameDecodeController::active_mode() const noexcept {
    return active_mode_;
}

FrameDecodeMode FrameDecodeController::requested_mode() const noexcept {
    return requested_mode_;
}

std::optional<FrameDecodeMode> FrameDecodeController::update(bool full_decode_needed,
                                                             bool keyframe_boundary,
                                                             Clock::time_point now) {
    if (idle_mode_ == FrameDecodeMode::all) {
        requested_mode_ = FrameDecodeMode::all;
        return std::nullopt;
    }

    if (full_decode_needed) {
        full_decode_until_ = now + grace_period_;
    }
    requested_mode_ =
        now < full_decode_until_ || full_decode_needed ? FrameDecodeMode::all : idle_mode_;

    if (requested_mode_ == FrameDecodeMode::all && active_mode_ == FrameDecodeMode::keyframes &&
        keyframe_boundary) {
        active_mode_ = FrameDecodeMode::all;
        return active_mode_;
    }
    if (requested_mode_ == FrameDecodeMode::keyframes && active_mode_ == FrameDecodeMode::all) {
        active_mode_ = FrameDecodeMode::keyframes;
        return active_mode_;
    }
    return std::nullopt;
}

} // namespace vibe_motion
