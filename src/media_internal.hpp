#pragma once

struct AVFrame;

namespace vibe_motion::detail {

bool decoded_frame_usable(const AVFrame* frame) noexcept;

} // namespace vibe_motion::detail
