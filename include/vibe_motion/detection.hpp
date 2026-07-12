#pragma once

#include "vibe_motion/frame.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace vibe_motion {

struct DetectionSettings {
    std::uint64_t threshold = 1500;
    std::uint8_t noise_level = 32;
    int lightswitch_percent = 0;
    bool despeckle = true;
    bool threshold_tune = false;
    double background_alpha = 0.04;
};

struct MotionRegion {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int center_x = 0;
    int center_y = 0;
};

struct DetectionResult {
    std::uint64_t changed_pixels = 0;
    bool motion = false;
    bool lightswitch_suppressed = false;
    std::uint64_t effective_threshold = 0;
    MotionRegion region{};
};

class MotionDetector {
  public:
    explicit MotionDetector(DetectionSettings settings);
    void set_mask(int width, int height, std::vector<std::uint8_t> enabled);
    void load_pgm_mask(const std::filesystem::path& path, int expected_width, int expected_height);
    DetectionResult process(const GrayFrame& frame);
    void reset();

  private:
    DetectionSettings settings_;
    int width_ = 0;
    int height_ = 0;
    std::vector<float> background_;
    std::vector<std::uint8_t> mask_;
    std::vector<std::uint8_t> changed_;
    std::size_t active_mask_pixels_ = 0;
    double background_changes_ = 0.0;
};

} // namespace vibe_motion
