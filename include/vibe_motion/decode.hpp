#pragma once

#include <chrono>
#include <optional>
#include <string_view>

namespace vibe_motion {

enum class FrameDecodeMode { all, keyframes };

std::string_view frame_decode_mode_name(FrameDecodeMode mode) noexcept;

// Camera-thread policy for temporarily enabling full decode while an HTTP
// consumer needs fresh JPEG frames. The decoder transition itself is returned
// to the caller so AVCodecContext remains owned by the camera thread.
class FrameDecodeController {
  public:
    using Clock = std::chrono::steady_clock;

    explicit FrameDecodeController(FrameDecodeMode idle_mode,
                                   std::chrono::milliseconds grace_period = std::chrono::seconds{
                                       5});

    FrameDecodeMode active_mode() const noexcept;
    FrameDecodeMode requested_mode() const noexcept;

    // A transition to full decode waits until the camera thread observes a
    // keyframe packet/frame boundary. Returning to keyframe-only decode happens
    // after the last consumer's grace period.
    std::optional<FrameDecodeMode> update(bool full_decode_needed, bool keyframe_boundary,
                                          Clock::time_point now);

  private:
    FrameDecodeMode idle_mode_;
    FrameDecodeMode active_mode_;
    FrameDecodeMode requested_mode_;
    std::chrono::milliseconds grace_period_;
    Clock::time_point full_decode_until_{};
};

} // namespace vibe_motion
