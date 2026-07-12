#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace vibe_motion {

struct GrayFrame {
    int width = 0;
    int height = 0;
    std::int64_t sequence = 0;
    std::chrono::system_clock::time_point captured_at{};
    std::vector<std::uint8_t> pixels;
};

} // namespace vibe_motion
