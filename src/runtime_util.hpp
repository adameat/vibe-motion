#pragma once

#include <algorithm>
#include <cctype>
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
                                                            bool selected_baichuan) {
    return configured_transport == "auto" && selected_baichuan;
}

} // namespace vibe_motion::runtime_detail
