#pragma once

struct AVFrame;

namespace vibe_motion {

class DecodedImage;

namespace detail {

struct DecodedImageAccess {
    static const AVFrame* frame(const DecodedImage& image) noexcept;
};

bool decoded_frame_usable(const AVFrame* frame) noexcept;
bool decoded_frame_has_color(const AVFrame* frame) noexcept;

} // namespace detail
} // namespace vibe_motion
