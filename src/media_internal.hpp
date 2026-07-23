#pragma once

#include <cstdint>
#include <optional>

struct AVFrame;

namespace vibe_motion::detail {

bool decoded_frame_usable(const AVFrame* frame) noexcept;

class PacketTimestampNormalizer {
  public:
    void reset(std::int64_t ticks_per_second) noexcept;
    std::int64_t normalize(std::optional<std::int64_t> input_timestamp,
                           std::int64_t arrival_timestamp) noexcept;

  private:
    std::int64_t ticks_per_second_ = 1;
    std::optional<std::int64_t> last_input_;
    std::int64_t first_arrival_ = 0;
    std::int64_t last_arrival_ = 0;
    std::int64_t last_output_ = 0;
    bool initialized_ = false;
};

} // namespace vibe_motion::detail
