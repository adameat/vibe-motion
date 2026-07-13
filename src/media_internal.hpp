#pragma once

struct AVFrame;

namespace vibe_motion {

class DecodedImage;

namespace detail {

bool decoded_frame_usable(const AVFrame* frame) noexcept;
bool decoded_image_has_color(const DecodedImage& image) noexcept;

} // namespace detail
} // namespace vibe_motion
