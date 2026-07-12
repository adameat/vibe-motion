#include "vibe_motion/detection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace vibe_motion {
namespace {

std::string pgm_token(std::istream& input) {
    std::string token;
    while (input >> token) {
        if (!token.empty() && token.front() == '#') {
            input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }
        return token;
    }
    throw std::runtime_error("unexpected end of PGM mask");
}

} // namespace

MotionDetector::MotionDetector(DetectionSettings settings) : settings_(settings) {
    if (settings_.background_alpha <= 0.0 || settings_.background_alpha > 1.0) {
        throw std::invalid_argument("background_alpha must be in (0, 1]");
    }
    effective_noise_level_ = static_cast<double>(settings_.noise_level);
}

void MotionDetector::set_mask(int width, int height, std::vector<std::uint8_t> enabled) {
    const auto pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || enabled.size() != pixel_count) {
        throw std::invalid_argument("mask dimensions do not match its data");
    }
    width_ = width;
    height_ = height;
    mask_ = std::move(enabled);
    active_mask_pixels_ = static_cast<std::size_t>(
        std::count_if(mask_.begin(), mask_.end(), [](std::uint8_t value) { return value != 0; }));
    background_.clear();
    effective_noise_level_ = static_cast<double>(settings_.noise_level);
    noise_estimate_initialized_ = false;
    upward_noise_frames_ = 0;
}

void MotionDetector::load_pgm_mask(const std::filesystem::path& path, int expected_width,
                                   int expected_height) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open mask: " + path.string());
    }
    const auto magic = pgm_token(input);
    const int width = std::stoi(pgm_token(input));
    const int height = std::stoi(pgm_token(input));
    const int maximum = std::stoi(pgm_token(input));
    if ((magic != "P5" && magic != "P2") || maximum <= 0 || maximum > 255) {
        throw std::runtime_error("only 8-bit P2/P5 PGM masks are supported: " + path.string());
    }
    if (width != expected_width || height != expected_height) {
        throw std::runtime_error("mask dimensions differ from configured camera dimensions: " +
                                 path.string());
    }
    std::vector<std::uint8_t> values(static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height));
    if (magic == "P5") {
        input.get();
        input.read(reinterpret_cast<char*>(values.data()),
                   static_cast<std::streamsize>(values.size()));
        if (input.gcount() != static_cast<std::streamsize>(values.size())) {
            throw std::runtime_error("truncated PGM mask: " + path.string());
        }
    } else {
        for (auto& value : values) {
            value = static_cast<std::uint8_t>(std::stoi(pgm_token(input)));
        }
    }
    set_mask(width, height, std::move(values));
}

void MotionDetector::reset() {
    background_.clear();
    changed_.clear();
    background_changes_ = 0.0;
    effective_noise_level_ = static_cast<double>(settings_.noise_level);
    noise_estimate_initialized_ = false;
    upward_noise_frames_ = 0;
}

DetectionResult MotionDetector::process(const GrayFrame& frame) {
    const auto pixel_count =
        static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.size() != pixel_count) {
        throw std::invalid_argument("invalid grayscale frame");
    }
    if (!mask_.empty() && (frame.width != width_ || frame.height != height_)) {
        throw std::runtime_error("frame dimensions changed after loading a mask");
    }
    width_ = frame.width;
    height_ = frame.height;
    if (background_.empty()) {
        background_.assign(frame.pixels.begin(), frame.pixels.end());
        changed_.resize(frame.pixels.size());
        DetectionResult result;
        result.effective_noise_level = settings_.noise_level;
        return result;
    }

    changed_.resize(frame.pixels.size());
    // Estimate codec/sensor noise from a bounded sample of raw background
    // differences. The median remains representative with a large moving
    // minority, while the multiplier leaves headroom above ordinary noise. A
    // local histogram keeps this allocation-free and cheap for large frames.
    double upward_noise_candidate = -1.0;
    if (settings_.noise_tune) {
        constexpr std::size_t target_samples = 4096;
        const auto stride = std::max<std::size_t>(1, pixel_count / target_samples);
        std::array<std::uint32_t, 256> histogram{};
        std::size_t samples = 0;
        for (std::size_t index = 0; index < pixel_count; index += stride) {
            if (!mask_.empty() && mask_[index] == 0) {
                continue;
            }
            float difference =
                std::abs(static_cast<float>(frame.pixels[index]) - background_[index]);
            if (!mask_.empty()) {
                difference *= static_cast<float>(mask_[index]) / 255.0F;
            }
            const auto bucket = static_cast<std::size_t>(
                std::clamp(static_cast<int>(std::lround(difference)), 0, 255));
            ++histogram[bucket];
            ++samples;
        }

        if (samples > 0) {
            const auto rank = (samples + 1) / 2;
            std::size_t cumulative = 0;
            std::size_t percentile = 0;
            for (; percentile < histogram.size(); ++percentile) {
                cumulative += histogram[percentile];
                if (cumulative >= rank) {
                    break;
                }
            }
            const double maximum = static_cast<double>(settings_.noise_level);
            // Keep quantization shimmer below the change map. In practice the
            // reference Motion estimator settles around 12 even on quiet
            // compressed feeds; going lower also makes lightswitch suppression
            // swallow large, legitimate foreground changes.
            const double minimum = std::min(12.0, maximum);
            const double candidate =
                std::clamp(12.0 + static_cast<double>(percentile) * 3.0, minimum, maximum);
            if (!noise_estimate_initialized_) {
                // The configured level is an initial safety ceiling. The first
                // representative frame should make tuning useful immediately.
                effective_noise_level_ = candidate;
                noise_estimate_initialized_ = true;
            } else if (candidate <= effective_noise_level_) {
                // Falling noise is safe to follow promptly.
                effective_noise_level_ += (candidate - effective_noise_level_) * 0.35;
                upward_noise_frames_ = 0;
            } else {
                upward_noise_candidate = candidate;
            }
        }
    } else {
        effective_noise_level_ = static_cast<double>(settings_.noise_level);
    }
    const auto effective_noise =
        static_cast<std::uint8_t>(std::clamp(std::lround(effective_noise_level_), 0L, 255L));
    const float noise_level = static_cast<float>(effective_noise);
    if (mask_.empty()) {
        // This is the overwhelmingly common path. Keep the mask test out of its
        // inner loop so every iteration performs only the pixel comparison.
        for (std::size_t index = 0; index < frame.pixels.size(); ++index) {
            const float difference =
                std::abs(static_cast<float>(frame.pixels[index]) - background_[index]);
            changed_[index] = static_cast<std::uint8_t>(difference > noise_level);
        }
    } else {
        for (std::size_t index = 0; index < frame.pixels.size(); ++index) {
            const auto mask_value = mask_[index];
            if (mask_value == 0) {
                changed_[index] = 0;
                continue;
            }
            float difference =
                std::abs(static_cast<float>(frame.pixels[index]) - background_[index]);
            difference *= static_cast<float>(mask_value) / 255.0F;
            changed_[index] = static_cast<std::uint8_t>(difference > noise_level);
        }
    }

    DetectionResult result;
    result.effective_noise_level = effective_noise;
    int min_x = width_;
    int min_y = height_;
    int max_x = -1;
    int max_y = -1;
    std::uint64_t sum_x = 0;
    std::uint64_t sum_y = 0;
    const auto accumulate_changed = [&](int x, int y) {
        ++result.changed_pixels;
        sum_x += static_cast<std::uint64_t>(x);
        sum_y += static_cast<std::uint64_t>(y);
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    };

    if (settings_.despeckle && width_ >= 3 && height_ >= 3) {
        for (int y = 0; y < height_; ++y) {
            const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
            if (y == 0 || y == height_ - 1) {
                for (int x = 0; x < width_; ++x) {
                    if (changed_[row + static_cast<std::size_t>(x)] != 0) {
                        accumulate_changed(x, y);
                    }
                }
                continue;
            }

            if (changed_[row] != 0) {
                accumulate_changed(0, y);
            }
            const auto previous_row = row - static_cast<std::size_t>(width_);
            const auto next_row = row + static_cast<std::size_t>(width_);
            for (int x = 1; x < width_ - 1; ++x) {
                const auto column = static_cast<std::size_t>(x);
                if (changed_[row + column] == 0) {
                    continue;
                }
                const int neighbors =
                    changed_[previous_row + column - 1] + changed_[previous_row + column] +
                    changed_[previous_row + column + 1] + changed_[row + column - 1] +
                    changed_[row + column + 1] + changed_[next_row + column - 1] +
                    changed_[next_row + column] + changed_[next_row + column + 1];
                if (neighbors >= 2) {
                    accumulate_changed(x, y);
                }
            }
            if (changed_[row + static_cast<std::size_t>(width_ - 1)] != 0) {
                accumulate_changed(width_ - 1, y);
            }
        }
    } else {
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                                   static_cast<std::size_t>(x);
                if (changed_[index] != 0) {
                    accumulate_changed(x, y);
                }
            }
        }
    }

    const auto active_pixels = mask_.empty() ? frame.pixels.size() : active_mask_pixels_;
    if (settings_.lightswitch_percent > 0 && active_pixels > 0 &&
        result.changed_pixels * 100U >=
            active_pixels * static_cast<unsigned>(settings_.lightswitch_percent)) {
        result.lightswitch_suppressed = true;
        result.changed_pixels = 0;
    } else if (result.changed_pixels > 0) {
        result.region = {min_x,
                         min_y,
                         max_x,
                         max_y,
                         static_cast<int>(sum_x / result.changed_pixels),
                         static_cast<int>(sum_y / result.changed_pixels)};
    }
    background_changes_ =
        background_changes_ == 0.0
            ? static_cast<double>(result.changed_pixels)
            : background_changes_ * 0.95 + static_cast<double>(result.changed_pixels) * 0.05;
    const auto base = settings_.threshold;
    const auto lower = std::max<std::uint64_t>(1, base / 4);
    const auto upper = std::max<std::uint64_t>(lower, base * 4);
    result.effective_threshold =
        settings_.threshold_tune
            ? std::clamp<std::uint64_t>(static_cast<std::uint64_t>(background_changes_ * 2.5),
                                        lower, upper)
            : base;
    result.motion = result.changed_pixels > result.effective_threshold;

    if (upward_noise_candidate >= 0.0) {
        // A single moving object or lightswitch can dominate even a robust
        // percentile. Suppress those frames, otherwise require persistence and
        // rise slowly so sustained codec/sensor noise still gets tracked.
        if (result.motion || result.lightswitch_suppressed) {
            upward_noise_frames_ = 0;
        } else {
            ++upward_noise_frames_;
            if (upward_noise_frames_ >= 5) {
                effective_noise_level_ += (upward_noise_candidate - effective_noise_level_) * 0.18;
            }
        }
    }

    const float alpha = static_cast<float>(result.motion ? settings_.background_alpha * 0.1
                                                         : settings_.background_alpha);
    for (std::size_t index = 0; index < frame.pixels.size(); ++index) {
        background_[index] +=
            alpha * (static_cast<float>(frame.pixels[index]) - background_[index]);
    }
    return result;
}

} // namespace vibe_motion
