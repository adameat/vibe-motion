#include "vibe_motion/decode.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace vibe_motion;
using namespace std::chrono_literals;

int main() {
    const auto start = FrameDecodeController::Clock::time_point{10s};

    FrameDecodeController always(FrameDecodeMode::all, 5s);
    assert(always.active_mode() == FrameDecodeMode::all);
    assert(!always.update(false, false, start));
    assert(!always.update(true, true, start + 1s));
    assert(always.requested_mode() == FrameDecodeMode::all);

    FrameDecodeController automatic(FrameDecodeMode::keyframes, 5s);
    assert(automatic.active_mode() == FrameDecodeMode::keyframes);
    assert(automatic.requested_mode() == FrameDecodeMode::keyframes);

    assert(!automatic.update(true, false, start));
    assert(automatic.requested_mode() == FrameDecodeMode::all);
    assert(automatic.active_mode() == FrameDecodeMode::keyframes);
    assert(!automatic.update(true, false, start + 1s));

    const auto enabled = automatic.update(true, true, start + 2s);
    assert(enabled && *enabled == FrameDecodeMode::all);
    assert(automatic.active_mode() == FrameDecodeMode::all);

    // Repeated demand extends the deadline, and short disconnects do not flap.
    assert(!automatic.update(true, false, start + 4s));
    assert(!automatic.update(false, false, start + 8s));
    assert(automatic.requested_mode() == FrameDecodeMode::all);
    assert(automatic.active_mode() == FrameDecodeMode::all);

    const auto disabled = automatic.update(false, false, start + 9s);
    assert(disabled && *disabled == FrameDecodeMode::keyframes);
    assert(automatic.requested_mode() == FrameDecodeMode::keyframes);
    assert(automatic.active_mode() == FrameDecodeMode::keyframes);

    // If demand disappears before an IDR arrives, the pending transition expires.
    FrameDecodeController abandoned(FrameDecodeMode::keyframes, 5s);
    assert(!abandoned.update(true, false, start));
    assert(!abandoned.update(false, false, start + 5s));
    assert(abandoned.requested_mode() == FrameDecodeMode::keyframes);
    assert(!abandoned.update(false, true, start + 6s));
    assert(abandoned.active_mode() == FrameDecodeMode::keyframes);

    assert(frame_decode_mode_name(FrameDecodeMode::all) == "all");
    assert(frame_decode_mode_name(FrameDecodeMode::keyframes) == "keyframes");

    std::cout << "decode tests passed\n";
}
