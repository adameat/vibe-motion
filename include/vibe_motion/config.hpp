#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace vibe_motion {

using OptionMap = std::map<std::string, std::string>;

struct CameraConfig {
    int camera_id = 0;
    std::string camera_name;
    std::string netcam_url;
    std::string netcam_userpass;
    std::string netcam_params;
    bool netcam_use_tcp = false;

    int width = 640;
    int height = 480;
    int framerate = 15;
    int text_scale = 1;
    bool text_changes = false;
    std::string text_left;
    std::string text_right = ".";

    bool emulate_motion = false;
    int threshold = 1500;
    bool threshold_tune = false;
    int noise_level = 32;
    std::string despeckle_filter;
    int lightswitch_percent = 0;
    int minimum_motion_frames = 1;
    int event_gap = 60;
    int pre_capture = 0;
    int post_capture = 0;
    std::string mask_file;

    bool picture_output = false;
    std::string picture_output_mode = "off";
    std::string picture_filename = "%v-%Y%m%d%H%M%S-%q";
    bool movie_output = true;
    bool movie_passthrough = false;
    bool movie_duplicate_frames = false;
    int movie_max_time = 0;
    int movie_quality = 75;
    std::string movie_container = "mkv";
    std::string movie_filename = "%v-%Y%m%d%H%M%S";

    int snapshot_interval = 0;
    std::string snapshot_filename = "snapshot";
    int timelapse_interval = 0;
    std::string timelapse_mode = "daily";
    std::string timelapse_filename = "%Y%m%d-timelapse";
    int timelapse_fps = 30;
    std::string timelapse_container = "mp4";
    std::string locate_motion_mode = "off";
    std::string locate_motion_style = "box";

    std::string on_event_start;
    std::string on_event_end;
    std::string on_picture_save;
    std::string on_movie_start;
    std::string on_movie_end;

    // Motion exposed one listener per camera. vibe-motion keeps these only for
    // compatibility and emits a warning; streaming is served by the main HTTP listener.
    int stream_port = 0;
    int stream_maxrate = 1;

    std::filesystem::path source_path;
    OptionMap unknown_options;
};

struct GlobalConfig {
    bool daemon = false;
    bool setup_mode = false;
    std::string log_file;
    int log_level = 6;
    std::string log_type = "ALL";
    int watchdog_tmo = 0;
    int watchdog_kill = 0;
    std::filesystem::path target_dir = ".";
    int webcontrol_port = 0;
    bool webcontrol_localhost = true;
    int webcontrol_parms = 0;
    std::string webcontrol_authentication;
    int stream_preview_scale = 25;
    std::string stream_preview_method = "mjpg";
    CameraConfig camera_defaults;
    OptionMap unknown_options;
};

struct Config {
    GlobalConfig global;
    std::vector<CameraConfig> cameras;
    std::vector<std::string> warnings;

    void validate() const;
    std::string dump_effective(bool redact_secrets = true) const;
};

struct ParseOptions {
    bool warn_unknown = true;
    bool reject_unknown = false;
};

class ConfigError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class ConfigParser {
  public:
    explicit ConfigParser(ParseOptions options = {});
    Config parse_file(const std::filesystem::path& path) const;
    Config parse_string(const std::string& text,
                        const std::filesystem::path& virtual_path = "<memory>") const;

  private:
    ParseOptions options_;
};

Config load_config(const std::filesystem::path& path, ParseOptions options = {});

struct ExpansionContext {
    int camera_id = 0;              // %t
    std::uint64_t event_number = 0; // %v, %{movienbr}, %{eventid}
    int threshold = 0;              // %o
    int changed_pixels = 0;         // %D
    int noise_level = 0;            // %N
    std::string filename;           // %f
    std::string file_type;          // %n
    std::uint64_t frame_number = 0; // %q
    int width = 0;                  // %w
    int height = 0;                 // %h
    std::string camera_name;        // %$
    std::string host;               // %{host}
    double fps = 0.0;               // %{fps}
};

std::string
expand_template(const std::string& pattern, const ExpansionContext& context,
                std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

std::filesystem::path
expand_path(const std::string& pattern, const std::filesystem::path& target_dir,
            const ExpansionContext& context,
            std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

} // namespace vibe_motion
