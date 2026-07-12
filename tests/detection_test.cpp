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

    // A partial mask retains its weighting semantics: a half-strength pixel
    // suppresses a difference that would pass with a fully enabled mask.
    detector.reset();
    auto weighted_mask = std::vector<std::uint8_t>(16, 0);
    weighted_mask[0] = 128;
    weighted_mask[1] = 255;
    detector.set_mask(4, 4, std::move(weighted_mask));
    assert(!detector.process(frame(4, 4, 10)).motion);
    auto weighted_change = frame(4, 4, 10);
    weighted_change.pixels[0] = 25;
    weighted_change.pixels[1] = 25;
    const auto weighted_result = detector.process(weighted_change);
    assert(weighted_result.changed_pixels == 1);
    assert(weighted_result.region.left == 1 && weighted_result.region.right == 1);

    // Despeckle removes an isolated interior point, retains an adjacent group,
    // and leaves border pixels untouched. Region and centroid use filtered data.
    settings.threshold = 2;
    settings.despeckle = true;
    MotionDetector despeckled(settings);
    assert(!despeckled.process(frame(5, 5, 0)).motion);
    auto noisy = frame(5, 5, 0);
    noisy.pixels[0] = 100; // Border pixels are not despeckled.
    noisy.pixels[6] = 100;
    noisy.pixels[7] = 100;
    noisy.pixels[11] = 100;
    noisy.pixels[18] = 100; // Isolated interior pixel is removed.
    const auto filtered = despeckled.process(noisy);
    assert(filtered.motion);
    assert(filtered.changed_pixels == 4);
    assert(filtered.region.left == 0 && filtered.region.top == 0);
    assert(filtered.region.right == 2 && filtered.region.bottom == 2);
    assert(filtered.region.center_x == 1 && filtered.region.center_y == 1);

    DetectionSettings lightswitch_settings;
    lightswitch_settings.noise_level = 10;
    lightswitch_settings.despeckle = false;
    lightswitch_settings.lightswitch_percent = 50;
    MotionDetector lightswitch(lightswitch_settings);
    std::vector<std::uint8_t> sparse_mask(16, 0);
    sparse_mask[0] = 255;
    sparse_mask[1] = 255;
    lightswitch.set_mask(4, 4, std::move(sparse_mask));
    assert(!lightswitch.process(frame(4, 4, 0)).motion);
    auto one_of_two = frame(4, 4, 0);
    one_of_two.pixels[0] = 255;
    const auto suppressed = lightswitch.process(one_of_two);
    assert(suppressed.lightswitch_suppressed);
    assert(suppressed.changed_pixels == 0);

    std::cout << "detection tests passed\n";
}
