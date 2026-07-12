#include "vibe_motion/detection.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace vibe_motion;

static GrayFrame frame(int width, int height, std::uint8_t value) {
    GrayFrame result;
    result.width = width;
    result.height = height;
    result.captured_at = std::chrono::system_clock::now();
    result.pixels.assign(static_cast<std::size_t>(width * height), value);
    return result;
}

int main() {
    DetectionSettings settings;
    settings.threshold = 3;
    settings.noise_level = 10;
    settings.despeckle = false;
    MotionDetector detector(settings);

    assert(!detector.process(frame(4, 4, 10)).motion);
    auto changed = frame(4, 4, 10);
    changed.pixels[0] = 100;
    changed.pixels[1] = 100;
    changed.pixels[4] = 100;
    changed.pixels[5] = 100;
    const auto detected = detector.process(changed);
    assert(detected.motion);
    assert(detected.changed_pixels == 4);
    assert(detected.region.left == 0 && detected.region.right == 1);

    detector.reset();
    detector.set_mask(4, 4, std::vector<std::uint8_t>(16, 0));
    assert(!detector.process(frame(4, 4, 0)).motion);
    assert(!detector.process(frame(4, 4, 255)).motion);

    std::cout << "detection tests passed\n";
}
