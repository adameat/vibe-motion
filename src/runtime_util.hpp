#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vibe_motion::runtime_detail {

inline std::string lowercase_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

inline bool starts_with_case_insensitive(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    return lowercase_ascii(value.substr(0, prefix.size())) == lowercase_ascii(prefix);
}

inline bool contains_case_insensitive(std::string_view value, std::string_view needle) {
    return lowercase_ascii(value).find(lowercase_ascii(needle)) != std::string::npos;
}

inline bool http_camera_url(std::string_view url) {
    return starts_with_case_insensitive(url, "http://") ||
           starts_with_case_insensitive(url, "https://");
}

inline bool onvif_identification_failure_is_fatal(bool onvif_client_present, std::string_view url) {
    return onvif_client_present && contains_case_insensitive(url, "/onvif/");
}

inline bool onvif_topic_holds_motion_state(std::string_view topic) {
    const std::string normalized = lowercase_ascii(topic);
    const auto slash = normalized.find_last_of("/:");
    const std::string_view leaf = slash == std::string::npos
                                      ? std::string_view(normalized)
                                      : std::string_view(normalized).substr(slash + 1);
    return leaf == "motion" || leaf == "motionalarm" || leaf == "cellmotiondetector";
}

class OnvifStateTracker {
  public:
    void update(std::string key, bool active, std::chrono::steady_clock::time_point now) {
        if (active) {
            // Repeated true notifications confirm the same state but do not extend its hard
            // lifetime. Only a false followed by a new true starts a fresh lease.
            active_since_.try_emplace(std::move(key), now);
        } else {
            active_since_.erase(key);
        }
    }

    std::vector<std::string> expire(std::chrono::steady_clock::time_point now,
                                    std::chrono::seconds timeout) {
        std::vector<std::string> expired;
        for (auto item = active_since_.begin(); item != active_since_.end();) {
            if (now - item->second >= timeout) {
                expired.push_back(item->first);
                item = active_since_.erase(item);
            } else {
                ++item;
            }
        }
        return expired;
    }

    bool active() const noexcept {
        return !active_since_.empty();
    }

    void clear() noexcept {
        active_since_.clear();
    }

  private:
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> active_since_;
};

inline bool auto_baichuan_open_failure_requires_reselection(std::string_view configured_transport,
                                                            bool selected_baichuan,
                                                            bool baichuan_established) {
    return configured_transport == "auto" && selected_baichuan && !baichuan_established;
}

inline std::string camera_thread_name(int camera_id, std::string_view role) {
    std::string result = "cam" + std::to_string(camera_id) + '-' + std::string(role);
    result.resize(std::min<std::size_t>(result.size(), 15));
    return result;
}

inline int snapshot_phase_seconds(int camera_id, int interval_seconds) {
    if (interval_seconds <= 0) {
        throw std::invalid_argument("snapshot interval must be positive");
    }
    const auto interval = static_cast<std::int64_t>(interval_seconds);
    // Multiplication keeps adjacent camera ids from occupying adjacent seconds.
    const auto camera = static_cast<std::int64_t>(camera_id) * 19;
    return static_cast<int>((camera % interval + interval) % interval);
}

inline std::int64_t snapshot_bucket_at(std::int64_t epoch_seconds, int interval_seconds,
                                       int camera_id) {
    const auto phase = snapshot_phase_seconds(camera_id, interval_seconds);
    return (epoch_seconds - phase) / interval_seconds;
}

inline std::string timelapse_period_key(std::chrono::system_clock::time_point when,
                                        std::string_view mode) {
    const auto instant = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    localtime_r(&instant, &local);

    std::string normalized_mode = lowercase_ascii(mode);
    const auto first = normalized_mode.find_first_not_of(" \t\r\n");
    const auto last = normalized_mode.find_last_not_of(" \t\r\n");
    normalized_mode = first == std::string::npos ? std::string{}
                                                 : normalized_mode.substr(first, last - first + 1);

    const char* format = nullptr;
    if (normalized_mode == "hourly") {
        format = "%Y%m%d%H";
    } else if (normalized_mode == "daily") {
        format = "%Y%m%d";
    } else {
        throw std::invalid_argument("unsupported timelapse rotation mode");
    }

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), format, &local);
    return buffer;
}

} // namespace vibe_motion::runtime_detail
