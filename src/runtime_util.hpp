#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <string>
#include <string_view>

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

inline bool auto_baichuan_open_failure_requires_reselection(std::string_view configured_transport,
                                                            bool selected_baichuan,
                                                            bool baichuan_established) {
    return configured_transport == "auto" && selected_baichuan && !baichuan_established;
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
