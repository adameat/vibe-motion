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
    settings.noise_tune = false;
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
    lightswitch_settings.noise_tune = false;
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

    // Adaptive noise tuning rapidly replaces Motion's deliberately generous
    // configured ceiling with a level representative of static codec noise.
    DetectionSettings adaptive_settings;
    adaptive_settings.threshold = 1000;
    adaptive_settings.noise_level = 128;
    adaptive_settings.despeckle = false;
    MotionDetector adaptive(adaptive_settings);
    const auto initial = adaptive.process(frame(64, 64, 100));
    assert(initial.effective_threshold == adaptive_settings.threshold);
    assert(initial.effective_noise_level == adaptive_settings.noise_level);
    auto codec_noise = frame(64, 64, 100);
    for (std::size_t index = 0; index < codec_noise.pixels.size(); ++index) {
        codec_noise.pixels[index] =
            static_cast<std::uint8_t>(100 + static_cast<int>(index % 11) - 5);
    }
    auto tuned = adaptive.process(codec_noise);
    assert(tuned.effective_noise_level >= 10);
    assert(tuned.effective_noise_level <= 24);

    // A transient large moving region must not inflate the learned level.
    const auto before_motion = tuned.effective_noise_level;
    auto large_motion = codec_noise;
    for (std::size_t index = 0; index < large_motion.pixels.size() * 3 / 5; ++index) {
        large_motion.pixels[index] = 220;
    }
    const auto during_motion = adaptive.process(large_motion);
    assert(during_motion.effective_noise_level == before_motion);
    assert(during_motion.motion);
    for (int iteration = 0; iteration < 8; ++iteration) {
        assert(adaptive.process(large_motion).effective_noise_level == before_motion);
    }

    // Persistently noisier input is eventually followed, but only after the
    // upward hysteresis has rejected short-lived motion.
    std::uint8_t noisier_level = during_motion.effective_noise_level;
    // Keep the test threshold above random-speckle activity: this models a
    // persistent noisy-but-static feed rather than a detected object.
    adaptive_settings.threshold = 10000;
    MotionDetector noisy_adaptive(adaptive_settings);
    noisy_adaptive.process(frame(64, 64, 100));
    noisy_adaptive.process(codec_noise);
    const auto quiet_level = noisy_adaptive.process(codec_noise).effective_noise_level;
    for (int iteration = 0; iteration < 16; ++iteration) {
        auto noisier = frame(64, 64, 100);
        const int sign = iteration % 2 == 0 ? 1 : -1;
        for (std::size_t index = 0; index < noisier.pixels.size(); ++index) {
            const int magnitude = 11 + static_cast<int>(index % 4);
            noisier.pixels[index] = static_cast<std::uint8_t>(100 + sign * magnitude);
        }
        noisier_level = noisy_adaptive.process(noisier).effective_noise_level;
    }
    assert(noisier_level > quiet_level);

    // With tuning disabled the configured level is exact and stable.
    DetectionSettings fixed_settings;
    fixed_settings.threshold = 10000;
    fixed_settings.noise_level = 37;
    fixed_settings.noise_tune = false;
    fixed_settings.despeckle = false;
    MotionDetector fixed(fixed_settings);
    assert(fixed.process(frame(16, 16, 80)).effective_noise_level == 37);
    assert(fixed.process(frame(16, 16, 120)).effective_noise_level == 37);

    std::cout << "detection tests passed\n";
}
