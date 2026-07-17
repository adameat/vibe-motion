#include "vibe_motion/config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace vibe_motion {
namespace {

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string unquote(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

struct LineOption {
    std::string key;
    std::string value;
};

LineOption split_option(const std::string& raw) {
    const std::string line = trim(raw);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
        return {};
    }
    const auto split = line.find_first_of(" \t=");
    if (split == std::string::npos) {
        return {lower(line), {}};
    }
    std::string value = trim(line.substr(split));
    if (!value.empty() && value.front() == '=') {
        value = trim(value.substr(1));
    }
    return {lower(line.substr(0, split)), value};
}

std::string where(const std::filesystem::path& path, std::size_t line) {
    return path.string() + ':' + std::to_string(line);
}

int integer(const std::string& value, const std::string& location, const std::string& key) {
    const std::string clean = trim(value);
    int result = 0;
    const auto parsed = std::from_chars(clean.data(), clean.data() + clean.size(), result);
    if (clean.empty() || parsed.ec != std::errc{} || parsed.ptr != clean.data() + clean.size()) {
        throw ConfigError(location + ": invalid integer for " + key + ": " + value);
    }
    return result;
}

bool boolean(const std::string& value, const std::string& location, const std::string& key) {
    const std::string clean = lower(trim(value));
    if (clean == "on" || clean == "yes" || clean == "true" || clean == "1") {
        return true;
    }
    if (clean == "off" || clean == "no" || clean == "false" || clean == "0") {
        return false;
    }
    throw ConfigError(location + ": invalid boolean for " + key + ": " + value);
}

std::string bool_text(bool value) {
    return value ? "on" : "off";
}

bool sensitive_key(const std::string& key) {
    const std::string name = lower(key);
    return name.find("password") != std::string::npos || name.find("passwd") != std::string::npos ||
           name.find("userpass") != std::string::npos ||
           name.find("authentication") != std::string::npos ||
           name.find("secret") != std::string::npos || name.find("token") != std::string::npos;
}

std::string redact_url(std::string url) {
    const auto scheme = url.find("://");
    if (scheme != std::string::npos) {
        const auto authority = scheme + 3;
        const auto end = url.find_first_of("/?#", authority);
        const auto at = url.find('@', authority);
        if (at != std::string::npos && (end == std::string::npos || at < end)) {
            url.replace(authority, at - authority, "REDACTED");
        }
    }
    std::string lowered = lower(url);
    for (const std::string key : {"user=", "username=", "password=", "pass=", "token="}) {
        std::size_t position = 0;
        while ((position = lowered.find(key, position)) != std::string::npos) {
            if (position == 0 || (url[position - 1] != '?' && url[position - 1] != '&')) {
                position += key.size();
                continue;
            }
            const auto value_begin = position + key.size();
            const auto value_end = url.find('&', value_begin);
            const auto count =
                (value_end == std::string::npos ? url.size() : value_end) - value_begin;
            url.replace(value_begin, count, "REDACTED");
            lowered = lower(url);
            position = value_begin + 8;
        }
    }
    return url;
}

enum class ApplyResult { applied, unknown };

ApplyResult apply_camera(CameraConfig& c, const std::string& original_key, const std::string& value,
                         const std::string& location, std::vector<std::string>& warnings) {
    std::string key = original_key;
    if (key == "device_id")
        key = "camera_id";
    if (key == "device_name")
        key = "camera_name";
    if (key == "lightswitch")
        key = "lightswitch_percent";
    if (key == "rtsp_uses_tcp")
        key = "netcam_use_tcp";

    // The distributed Motion configuration keeps device_id/device_name as
    // active-but-empty defaults. An empty id means "not set" and is valid in
    // the main file as long as every included camera supplies one.
    if (key == "camera_id")
        c.camera_id = trim(value).empty() ? 0 : integer(value, location, key);
    else if (key == "camera_name")
        c.camera_name = value;
    else if (key == "netcam_url")
        c.netcam_url = value;
    else if (key == "netcam_userpass")
        c.netcam_userpass = value;
    else if (key == "netcam_params")
        c.netcam_params = value;
    else if (key == "netcam_use_tcp")
        c.netcam_use_tcp = boolean(value, location, key);
    else if (key == "onvif_url")
        c.onvif_url = value;
    else if (key == "onvif_userpass")
        c.onvif_userpass = value;
    else if (key == "onvif_profile")
        c.onvif_profile = value;
    else if (key == "onvif_auth")
        c.onvif_auth = lower(trim(value));
    else if (key == "onvif_events")
        c.onvif_events = boolean(value, location, key);
    else if (key == "onvif_log_events")
        c.onvif_log_events = boolean(value, location, key);
    else if (key == "onvif_tls_verify")
        c.onvif_tls_verify = boolean(value, location, key);
    else if (key == "onvif_motion_topics")
        c.onvif_motion_topics = value;
    else if (key == "motion_detection")
        c.motion_detection = boolean(value, location, key);
    else if (key == "width") {
        if (lower(trim(value)) == "auto") {
            c.width = 640;
            c.width_configured = false;
        } else {
            c.width = integer(value, location, key);
            c.width_configured = true;
        }
    } else if (key == "height") {
        if (lower(trim(value)) == "auto") {
            c.height = 480;
            c.height_configured = false;
        } else {
            c.height = integer(value, location, key);
            c.height_configured = true;
        }
    } else if (key == "framerate")
        c.framerate = integer(value, location, key);
    else if (key == "text_scale")
        c.text_scale = integer(value, location, key);
    else if (key == "text_changes")
        c.text_changes = boolean(value, location, key);
    else if (key == "text_left")
        c.text_left = value;
    else if (key == "text_right")
        c.text_right = value;
    else if (key == "emulate_motion")
        c.emulate_motion = boolean(value, location, key);
    else if (key == "threshold")
        c.threshold = integer(value, location, key);
    else if (key == "threshold_tune")
        c.threshold_tune = boolean(value, location, key);
    else if (key == "noise_level")
        c.noise_level = integer(value, location, key);
    else if (key == "noise_tune")
        c.noise_tune = boolean(value, location, key);
    else if (key == "despeckle_filter")
        c.despeckle_filter = value;
    else if (key == "lightswitch_percent")
        c.lightswitch_percent = integer(value, location, key);
    else if (key == "minimum_motion_frames")
        c.minimum_motion_frames = integer(value, location, key);
    else if (key == "event_gap")
        c.event_gap = integer(value, location, key);
    else if (key == "pre_capture")
        c.pre_capture = integer(value, location, key);
    else if (key == "post_capture")
        c.post_capture = integer(value, location, key);
    else if (key == "mask_file")
        c.mask_file = value;
    else if (key == "picture_output") {
        c.picture_output_mode = lower(trim(value));
        if (c.picture_output_mode == "on" || c.picture_output_mode == "yes" ||
            c.picture_output_mode == "true" || c.picture_output_mode == "1" ||
            c.picture_output_mode == "best" || c.picture_output_mode == "first" ||
            c.picture_output_mode == "center")
            c.picture_output = true;
        else if (c.picture_output_mode == "off" || c.picture_output_mode == "no" ||
                 c.picture_output_mode == "false" || c.picture_output_mode == "0")
            c.picture_output = false;
        else
            throw ConfigError(location + ": invalid picture_output mode: " + value);
    } else if (key == "picture_filename")
        c.picture_filename = value;
    else if (key == "movie_output")
        c.movie_output = boolean(value, location, key);
    else if (key == "movie_passthrough")
        c.movie_passthrough = boolean(value, location, key);
    else if (key == "movie_all_frames")
        c.movie_all_frames = boolean(value, location, key);
    else if (key == "movie_duplicate_frames")
        c.movie_duplicate_frames = boolean(value, location, key);
    else if (key == "movie_max_time")
        c.movie_max_time = integer(value, location, key);
    else if (key == "movie_quality")
        c.movie_quality = integer(value, location, key);
    else if (key == "movie_container")
        c.movie_container = value;
    else if (key == "movie_filename")
        c.movie_filename = value;
    else if (key == "snapshot_interval")
        c.snapshot_interval = integer(value, location, key);
    else if (key == "snapshot_filename")
        c.snapshot_filename = value;
    else if (key == "timelapse_interval")
        c.timelapse_interval = integer(value, location, key);
    else if (key == "timelapse_mode")
        c.timelapse_mode = value;
    else if (key == "timelapse_filename")
        c.timelapse_filename = value;
    else if (key == "timelapse_fps")
        c.timelapse_fps = integer(value, location, key);
    else if (key == "timelapse_container")
        c.timelapse_container = value;
    else if (key == "locate_motion_mode")
        c.locate_motion_mode = lower(trim(value));
    else if (key == "locate_motion_style")
        c.locate_motion_style = lower(trim(value));
    else if (key == "on_event_start")
        c.on_event_start = value;
    else if (key == "on_event_end")
        c.on_event_end = value;
    else if (key == "on_picture_save")
        c.on_picture_save = value;
    else if (key == "on_movie_start")
        c.on_movie_start = value;
    else if (key == "on_movie_end")
        c.on_movie_end = value;
    else if (key == "stream_port") {
        c.stream_port = integer(value, location, key);
        warnings.push_back(
            location +
            ": stream_port is accepted for compatibility but uses the global HTTP listener");
    } else if (key == "stream_maxrate") {
        c.stream_maxrate = integer(value, location, key);
    } else
        return ApplyResult::unknown;
    return ApplyResult::applied;
}

ApplyResult apply_global(GlobalConfig& g, const std::string& key, const std::string& value,
                         const std::string& location) {
    if (key == "daemon")
        g.daemon = boolean(value, location, key);
    else if (key == "setup_mode")
        g.setup_mode = boolean(value, location, key);
    else if (key == "log_file")
        g.log_file = value;
    else if (key == "log_level")
        g.log_level = integer(value, location, key);
    else if (key == "log_type")
        g.log_type = value;
    else if (key == "watchdog_tmo")
        g.watchdog_tmo = integer(value, location, key);
    else if (key == "watchdog_kill")
        g.watchdog_kill = integer(value, location, key);
    else if (key == "target_dir")
        g.target_dir = unquote(value);
    else if (key == "webcontrol_port")
        g.webcontrol_port = integer(value, location, key);
    else if (key == "webcontrol_localhost")
        g.webcontrol_localhost = boolean(value, location, key);
    else if (key == "webcontrol_parms")
        g.webcontrol_parms = integer(value, location, key);
    else if (key == "webcontrol_authentication")
        g.webcontrol_authentication = value;
    else if (key == "stream_preview_scale")
        g.stream_preview_scale = integer(value, location, key);
    else if (key == "stream_preview_method")
        g.stream_preview_method = value;
    else
        return ApplyResult::unknown;
    return ApplyResult::applied;
}

class Parser {
  public:
    explicit Parser(ParseOptions options) : options_(options) {}

    Config parse_file(const std::filesystem::path& path) {
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        std::ifstream input(absolute);
        if (!input)
            throw ConfigError("cannot open configuration file: " + absolute.string());
        parse_main(input, absolute);
        config_.validate();
        return config_;
    }

    Config parse_string(const std::string& text, const std::filesystem::path& virtual_path) {
        std::istringstream input(text);
        parse_main(input, virtual_path);
        config_.validate();
        return config_;
    }

  private:
    void unknown(OptionMap& map, const LineOption& option, const std::string& location) {
        map[option.key] = option.value;
        if (options_.reject_unknown) {
            throw ConfigError(location + ": unsupported option: " + option.key);
        }
        if (options_.warn_unknown) {
            config_.warnings.push_back(location + ": unsupported option preserved: " + option.key);
        }
    }

    void parse_main(std::istream& input, const std::filesystem::path& path) {
        std::string raw;
        std::size_t line_number = 0;
        while (std::getline(input, raw)) {
            ++line_number;
            const LineOption option = split_option(raw);
            if (option.key.empty())
                continue;
            const std::string location = where(path, line_number);
            if (option.key == "camera") {
                if (trim(option.value).empty())
                    throw ConfigError(location + ": camera requires a file path");
                std::filesystem::path include = unquote(option.value);
                if (include.is_relative())
                    include = path.parent_path() / include;
                include = std::filesystem::absolute(include).lexically_normal();
                CameraConfig camera = config_.global.camera_defaults;
                camera.source_path = include;
                parse_camera_file(camera, include, location);
                config_.cameras.push_back(std::move(camera));
                continue;
            }
            if (apply_global(config_.global, option.key, option.value, location) ==
                ApplyResult::applied) {
                continue;
            }
            if (apply_camera(config_.global.camera_defaults, option.key, option.value, location,
                             config_.warnings) == ApplyResult::applied) {
                continue;
            }
            unknown(config_.global.unknown_options, option, location);
            config_.global.camera_defaults.unknown_options[option.key] = option.value;
        }
    }

    void parse_camera_file(CameraConfig& camera, const std::filesystem::path& path,
                           const std::string& include_location) {
        const std::string key = path.string();
        if (!include_stack_.insert(key).second) {
            throw ConfigError(include_location + ": recursive camera include: " + key);
        }
        std::ifstream input(path);
        if (!input) {
            include_stack_.erase(key);
            throw ConfigError(include_location + ": cannot open camera file: " + key);
        }
        std::string raw;
        std::size_t line_number = 0;
        while (std::getline(input, raw)) {
            ++line_number;
            const LineOption option = split_option(raw);
            if (option.key.empty())
                continue;
            const std::string location = where(path, line_number);
            if (option.key == "camera") {
                include_stack_.erase(key);
                throw ConfigError(location + ": nested camera directives are not supported");
            }
            if (apply_camera(camera, option.key, option.value, location, config_.warnings) ==
                ApplyResult::unknown) {
                unknown(camera.unknown_options, option, location);
            }
        }
        include_stack_.erase(key);
    }

    ParseOptions options_;
    Config config_;
    std::set<std::string> include_stack_;
};

void check_range(bool valid, const CameraConfig& c, const std::string& text) {
    if (!valid) {
        const std::string camera =
            c.camera_name.empty() ? std::to_string(c.camera_id) : c.camera_name;
        throw ConfigError("camera " + camera + ": " + text);
    }
}

void dump_unknown(std::ostringstream& out, const OptionMap& values, bool redact) {
    for (const auto& item : values) {
        out << item.first << ' '
            << ((redact && sensitive_key(item.first)) ? "REDACTED" : item.second) << '\n';
    }
}

void dump_camera(std::ostringstream& out, const CameraConfig& c, bool redact) {
    out << "camera_id " << c.camera_id << '\n'
        << "camera_name " << c.camera_name << '\n'
        << "netcam_url " << (redact ? redact_url(c.netcam_url) : c.netcam_url) << '\n'
        << "netcam_userpass "
        << (redact && !c.netcam_userpass.empty() ? "REDACTED" : c.netcam_userpass) << '\n'
        << "netcam_params " << c.netcam_params << '\n'
        << "netcam_use_tcp " << bool_text(c.netcam_use_tcp) << '\n'
        << "onvif_url " << (redact ? redact_url(c.onvif_url) : c.onvif_url) << '\n'
        << "onvif_userpass "
        << (redact && !c.onvif_userpass.empty() ? "REDACTED" : c.onvif_userpass) << '\n'
        << "onvif_profile " << c.onvif_profile << '\n'
        << "onvif_auth " << c.onvif_auth << '\n'
        << "onvif_events " << bool_text(c.onvif_events) << '\n'
        << "onvif_log_events " << bool_text(c.onvif_log_events) << '\n'
        << "onvif_tls_verify " << bool_text(c.onvif_tls_verify) << '\n'
        << "onvif_motion_topics " << c.onvif_motion_topics << '\n'
        << "motion_detection " << bool_text(c.motion_detection) << '\n'
        << "width "
        << (!c.onvif_url.empty() && !c.width_configured ? "auto" : std::to_string(c.width)) << '\n'
        << "height "
        << (!c.onvif_url.empty() && !c.height_configured ? "auto" : std::to_string(c.height))
        << '\n'
        << "framerate " << c.framerate << '\n'
        << "text_scale " << c.text_scale << '\n'
        << "text_changes " << bool_text(c.text_changes) << '\n'
        << "text_left " << c.text_left << '\n'
        << "text_right " << c.text_right << '\n'
        << "emulate_motion " << bool_text(c.emulate_motion) << '\n'
        << "threshold " << c.threshold << '\n'
        << "threshold_tune " << bool_text(c.threshold_tune) << '\n'
        << "noise_level " << c.noise_level << '\n'
        << "noise_tune " << bool_text(c.noise_tune) << '\n'
        << "despeckle_filter " << c.despeckle_filter << '\n'
        << "lightswitch_percent " << c.lightswitch_percent << '\n'
        << "minimum_motion_frames " << c.minimum_motion_frames << '\n'
        << "event_gap " << c.event_gap << '\n'
        << "pre_capture " << c.pre_capture << '\n'
        << "post_capture " << c.post_capture << '\n'
        << "mask_file " << c.mask_file << '\n'
        << "picture_output " << c.picture_output_mode << '\n'
        << "picture_filename " << c.picture_filename << '\n'
        << "movie_output " << bool_text(c.movie_output) << '\n'
        << "movie_passthrough " << bool_text(c.movie_passthrough) << '\n'
        << "movie_all_frames " << bool_text(c.movie_all_frames) << '\n'
        << "movie_duplicate_frames " << bool_text(c.movie_duplicate_frames) << '\n'
        << "movie_max_time " << c.movie_max_time << '\n'
        << "movie_quality " << c.movie_quality << '\n'
        << "movie_container " << c.movie_container << '\n'
        << "movie_filename " << c.movie_filename << '\n'
        << "snapshot_interval " << c.snapshot_interval << '\n'
        << "snapshot_filename " << c.snapshot_filename << '\n'
        << "timelapse_interval " << c.timelapse_interval << '\n'
        << "timelapse_mode " << c.timelapse_mode << '\n'
        << "timelapse_filename " << c.timelapse_filename << '\n'
        << "timelapse_fps " << c.timelapse_fps << '\n'
        << "timelapse_container " << c.timelapse_container << '\n'
        << "locate_motion_mode " << c.locate_motion_mode << '\n'
        << "locate_motion_style " << c.locate_motion_style << '\n'
        << "on_event_start " << c.on_event_start << '\n'
        << "on_event_end " << c.on_event_end << '\n'
        << "on_picture_save " << c.on_picture_save << '\n'
        << "on_movie_start " << c.on_movie_start << '\n'
        << "on_movie_end " << c.on_movie_end << '\n'
        << "stream_port " << c.stream_port << '\n'
        << "stream_maxrate " << c.stream_maxrate << '\n';
    dump_unknown(out, c.unknown_options, redact);
}

std::string padded_event(std::uint64_t value) {
    std::ostringstream out;
    out << std::setw(2) << std::setfill('0') << value;
    return out.str();
}

std::string fps_text(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    std::string result = out.str();
    while (result.size() > 1 && result.back() == '0')
        result.pop_back();
    if (!result.empty() && result.back() == '.')
        result.pop_back();
    return result;
}

void append_strftime_literal(std::string& output, const std::string& value) {
    for (const char ch : value) {
        // Custom values are data, not another layer of strftime syntax.
        if (ch == '%')
            output.push_back('%');
        output.push_back(ch);
    }
}

} // namespace

std::string timelapse_file_extension(std::string container) {
    container = lower(trim(std::move(container)));
    if (container == "mkv") {
        return ".mkv";
    }
    if (container == "mpeg4") {
        return ".avi";
    }
    throw ConfigError("timelapse_container must be mkv or mpeg4");
}

ConfigParser::ConfigParser(ParseOptions options) : options_(options) {}

Config ConfigParser::parse_file(const std::filesystem::path& path) const {
    return Parser(options_).parse_file(path);
}

Config ConfigParser::parse_string(const std::string& text,
                                  const std::filesystem::path& virtual_path) const {
    return Parser(options_).parse_string(text, virtual_path);
}

Config load_config(const std::filesystem::path& path, ParseOptions options) {
    return ConfigParser(options).parse_file(path);
}

void Config::validate() const {
    if (global.log_level < 0 || global.log_level > 9)
        throw ConfigError("log_level must be between 0 and 9");
    if (global.webcontrol_port < 0 || global.webcontrol_port > 65535)
        throw ConfigError("invalid webcontrol_port");
    if (global.stream_preview_scale < 1 || global.stream_preview_scale > 100) {
        throw ConfigError("stream_preview_scale must be between 1 and 100");
    }
    if (!global.webcontrol_authentication.empty()) {
        throw ConfigError("webcontrol_authentication is not supported by this build");
    }
    if (global.webcontrol_parms != 0) {
        throw ConfigError("only read-only webcontrol_parms 0 is supported");
    }
    std::set<int> ids;
    std::set<std::string> names;
    for (const CameraConfig& c : cameras) {
        check_range(c.camera_id > 0, c, "camera_id must be positive");
        check_range(!trim(c.camera_name).empty(), c, "camera_name is required");
        check_range(ids.insert(c.camera_id).second, c, "duplicate camera_id");
        check_range(names.insert(c.camera_name).second, c, "duplicate camera_name");
        const std::string url = lower(trim(c.netcam_url));
        const std::string onvif_url = lower(trim(c.onvif_url));
        const bool valid_media_url = url.rfind("rtsp://", 0) == 0 || url.rfind("rtmp://", 0) == 0 ||
                                     url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
        const bool valid_onvif_url =
            onvif_url.rfind("http://", 0) == 0 || onvif_url.rfind("https://", 0) == 0;
        check_range(valid_media_url || valid_onvif_url, c,
                    "netcam_url or an HTTP(S) onvif_url is required");
        check_range(onvif_url.empty() || valid_onvif_url, c, "onvif_url must use http or https");
        check_range(!c.onvif_events || valid_onvif_url, c, "onvif_events requires onvif_url");
        check_range(!c.onvif_log_events || c.onvif_events, c,
                    "onvif_log_events requires onvif_events");
        check_range(!c.onvif_events || !trim(c.onvif_motion_topics).empty(), c,
                    "onvif_motion_topics cannot be empty when events are enabled");
        check_range(c.onvif_auth == "auto" || c.onvif_auth == "digest" || c.onvif_auth == "wsse", c,
                    "onvif_auth must be auto, digest, or wsse");
        check_range(c.width > 0 && c.height > 0, c, "width and height must be positive");
        check_range(c.framerate > 0 && c.framerate <= 240, c,
                    "framerate must be between 1 and 240");
        check_range(c.text_scale > 0, c, "text_scale must be positive");
        check_range(!c.emulate_motion, c, "emulate_motion is not implemented");
        check_range(c.threshold >= 0, c, "threshold cannot be negative");
        check_range(c.noise_level >= 0 && c.noise_level <= 255, c,
                    "noise_level must be between 0 and 255");
        check_range(c.lightswitch_percent >= 0 && c.lightswitch_percent <= 100, c,
                    "lightswitch_percent must be between 0 and 100");
        check_range(c.minimum_motion_frames > 0, c, "minimum_motion_frames must be positive");
        check_range(c.event_gap >= 0 && c.pre_capture >= 0 && c.post_capture >= 0, c,
                    "capture and event intervals cannot be negative");
        check_range(c.movie_quality >= 0 && c.movie_quality <= 100, c,
                    "movie_quality must be between 0 and 100");
        check_range(!c.picture_output || c.picture_output_mode == "best", c,
                    "only picture_output off/best is implemented");
        check_range(!c.movie_output || c.movie_passthrough, c,
                    "movie_output currently requires movie_passthrough on");
        check_range(c.movie_max_time == 0, c, "movie_max_time rollover is not implemented; use 0");
        check_range(c.movie_container == "mp4" || c.movie_container == "mkv", c,
                    "movie_container must be mp4 or mkv");
        check_range(c.movie_max_time >= 0 && c.snapshot_interval >= 0 && c.timelapse_interval >= 0,
                    c, "output intervals cannot be negative");
        check_range(c.timelapse_fps > 0 && c.timelapse_fps <= 240, c,
                    "timelapse_fps must be between 1 and 240");
        check_range(c.timelapse_interval == 0 || lower(c.timelapse_mode) == "hourly", c,
                    "only hourly timelapse_mode is implemented");
        const std::string timelapse_container = lower(trim(c.timelapse_container));
        check_range(c.timelapse_interval == 0 || timelapse_container == "mkv" ||
                        timelapse_container == "mpeg4",
                    c, "timelapse_container must be mkv or mpeg4");
        check_range(c.stream_port >= 0 && c.stream_port <= 65535, c, "invalid stream_port");
        check_range(c.stream_maxrate > 0, c, "stream_maxrate must be positive");
    }
}

std::string Config::dump_effective(bool redact) const {
    std::ostringstream out;
    out << "daemon " << bool_text(global.daemon) << '\n'
        << "setup_mode " << bool_text(global.setup_mode) << '\n'
        << "log_file " << global.log_file << '\n'
        << "log_level " << global.log_level << '\n'
        << "log_type " << global.log_type << '\n'
        << "watchdog_tmo " << global.watchdog_tmo << '\n'
        << "watchdog_kill " << global.watchdog_kill << '\n'
        << "target_dir " << global.target_dir.string() << '\n'
        << "webcontrol_port " << global.webcontrol_port << '\n'
        << "webcontrol_localhost " << bool_text(global.webcontrol_localhost) << '\n'
        << "webcontrol_parms " << global.webcontrol_parms << '\n'
        << "webcontrol_authentication "
        << (redact && !global.webcontrol_authentication.empty() ? "REDACTED"
                                                                : global.webcontrol_authentication)
        << '\n'
        << "stream_preview_scale " << global.stream_preview_scale << '\n'
        << "stream_preview_method " << global.stream_preview_method << '\n';
    dump_unknown(out, global.unknown_options, redact);
    for (std::size_t i = 0; i < cameras.size(); ++i) {
        out << "\n# effective camera " << (i + 1) << " from " << cameras[i].source_path.string()
            << '\n';
        dump_camera(out, cameras[i], redact);
    }
    return out.str();
}

std::string expand_template(const std::string& pattern, const ExpansionContext& c,
                            std::chrono::system_clock::time_point when) {
    std::string expanded;
    expanded.reserve(pattern.size() + 32);
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] != '%' || i + 1 >= pattern.size()) {
            expanded.push_back(pattern[i++]);
            continue;
        }
        if (pattern[i + 1] == '{') {
            const auto close = pattern.find('}', i + 2);
            if (close != std::string::npos) {
                const std::string name = lower(pattern.substr(i + 2, close - i - 2));
                if (name == "host")
                    append_strftime_literal(expanded, c.host);
                else if (name == "fps")
                    expanded += fps_text(c.fps);
                else if (name == "movienbr" || name == "eventid")
                    expanded += std::to_string(c.event_number);
                else {
                    expanded.append(pattern, i, close - i + 1);
                }
                i = close + 1;
                continue;
            }
        }
        const char token = pattern[i + 1];
        bool handled = true;
        switch (token) {
        case 't':
            expanded += std::to_string(c.camera_id);
            break;
        case 'v':
            expanded += padded_event(c.event_number);
            break;
        case 'o':
            expanded += std::to_string(c.threshold);
            break;
        case 'D':
            expanded += std::to_string(c.changed_pixels);
            break;
        case 'N':
            expanded += std::to_string(c.noise_level);
            break;
        case 'f':
            append_strftime_literal(expanded, c.filename);
            break;
        case 'n':
            append_strftime_literal(expanded, c.file_type);
            break;
        case 'q':
            expanded += std::to_string(c.frame_number);
            break;
        case 'w':
            expanded += std::to_string(c.width);
            break;
        case 'h':
            expanded += std::to_string(c.height);
            break;
        case '$':
            append_strftime_literal(expanded, c.camera_name);
            break;
        default:
            handled = false;
            break;
        }
        if (handled)
            i += 2;
        else
            expanded.push_back(pattern[i++]);
    }

    const std::time_t raw_time = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &raw_time);
#else
    localtime_r(&raw_time, &local);
#endif
    std::vector<char> buffer(std::max<std::size_t>(256, expanded.size() * 2 + 1));
    while (buffer.size() <= std::size_t{1024} * 1024) {
        const std::size_t size =
            std::strftime(buffer.data(), buffer.size(), expanded.c_str(), &local);
        if (size != 0)
            return std::string(buffer.data(), size);
        if (expanded.empty())
            return {};
        buffer.resize(buffer.size() * 2);
    }
    throw ConfigError("expanded template is too large");
}

std::filesystem::path expand_path(const std::string& pattern,
                                  const std::filesystem::path& target_dir,
                                  const ExpansionContext& context,
                                  std::chrono::system_clock::time_point when) {
    std::filesystem::path path = expand_template(pattern, context, when);
    if (path.is_relative())
        path = target_dir / path;
    return path.lexically_normal();
}

} // namespace vibe_motion
