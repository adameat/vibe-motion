#pragma once

#include <cstdint>

namespace vibe_motion::detail {

// Versions are contiguous per camera AND stream kind, not server-global.
inline bool packet_window_overrun(std::uint64_t delivered, std::uint64_t oldest_retained) {
    return oldest_retained > delivered && oldest_retained - delivered > 1;
}

} // namespace vibe_motion::detail
