#include "vibe_motion/config.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

using namespace vibe_motion;

namespace {

template <typename Function> void expect_config_error(Function function) {
    bool thrown = false;
    try {
        function();
    } catch (const ConfigError&) {
        thrown = true;
    }
    assert(thrown);
}

} // namespace

int main() {
    assert(timelapse_file_extension("mkv") == ".mkv");
    assert(timelapse_file_extension(" MPEG4 ") == ".avi");
    expect_config_error([] { timelapse_file_extension("mpg"); });

    const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures";
    const Config config = load_config(fixtures / "motion.conf");

    assert(config.global.webcontrol_port == 8880);
    assert(!config.global.webcontrol_localhost);
    assert(config.global.target_dir == "output");
    assert(config.global.camera_defaults.width == 1280);
    assert(config.global.camera_defaults.lightswitch_percent == 50);
    assert(config.global.unknown_options.at("future_global") == "retained value");
    assert(config.cameras.size() == 2);

    const CameraConfig& first = config.cameras.at(0);
    assert(first.camera_id == 3);
    assert(first.camera_name == "First camera");
    assert(first.width == 800); // cloned when the camera directive was encountered
    assert(first.height == 600);
    assert(first.netcam_use_tcp);
    assert(first.picture_output && first.picture_output_mode == "best");
    assert(first.on_event_start == "/hooks/start %t %v %$");
    assert(first.unknown_options.at("future_global") == "retained value");
    assert(first.unknown_options.at("future_camera") == "one two three");
    assert(first.source_path.filename() == "first.conf");

    const CameraConfig& second = config.cameras.at(1);
    assert(second.camera_id == 4);
    assert(second.width == 1280);
    assert(!second.netcam_use_tcp);
    assert(second.movie_quality == 80);
    assert(!config.warnings.empty());

    const Config deployment = load_config(fixtures / "deployment" / "motion.conf");
    assert(deployment.cameras.size() == 3);
    assert(deployment.cameras.front().camera_id == 1);
    assert(deployment.cameras.back().camera_id == 3);
    assert(deployment.cameras.front().netcam_url.rfind("rtmp://", 0) == 0);
    assert(deployment.global.webcontrol_port == 8880);
    assert(deployment.global.camera_defaults.threshold_tune);
    assert(deployment.global.camera_defaults.noise_tune);
    assert(deployment.global.camera_defaults.movie_all_frames);
    assert(deployment.cameras.front().locate_motion_mode == "preview");
    assert(deployment.cameras.front().locate_motion_style == "redbox");

    const std::string safe = config.dump_effective();
    assert(safe.find("alice:secret") == std::string::npos);
    assert(safe.find("rtsp://REDACTED@example.test/live") != std::string::npos);
    assert(safe.find("future_camera one two three") != std::string::npos);
    const std::string unsafe = config.dump_effective(false);
    assert(unsafe.find("alice:secret") != std::string::npos);

    const Config rtmp =
        ConfigParser().parse_string("camera cameras/rtmp.conf\n", fixtures / "rtmp-main.conf");
    const std::string rtmp_dump = rtmp.dump_effective();
    assert(rtmp_dump.find("alice") == std::string::npos);
    assert(rtmp_dump.find("secret") == std::string::npos);

    ExpansionContext context;
    context.camera_id = 3;
    context.event_number = 7;
    context.threshold = 6000;
    context.changed_pixels = 123;
    context.noise_level = 44;
    context.filename = "/tmp/movie.mp4";
    context.file_type = "8";
    context.frame_number = 19;
    context.width = 2560;
    context.height = 1920;
    context.camera_name = "camera03";
    context.host = "test-host";
    context.fps = 5.25;
    const auto timestamp = std::chrono::system_clock::time_point{std::chrono::seconds{1704196800}};
    const std::string expanded = expand_template(
        "%t/%v/%o/%D/%N/%f/%n/%q/%w/%h/%$/%{host}/%{fps}/%{movienbr}/%{eventid}/%Y%m%d/%%", context,
        timestamp);
    assert(expanded ==
           "3/07/6000/123/44//tmp/movie.mp4/8/19/2560/1920/camera03/test-host/5.25/7/7/20240102/%");
    assert(expand_path("events/%$/%v.mp4", "/cam", context, timestamp) ==
           std::filesystem::path("/cam/events/camera03/07.mp4"));

    const Config comments = ConfigParser().parse_string(
        "# ignored\n; ignored too\ndaemon=yes\nfuture=value with spaces\n");
    assert(comments.global.daemon);
    assert(comments.global.unknown_options.at("future") == "value with spaces");

    const Config noise = ConfigParser().parse_string(
        "noise_level 64\nnoise_tune off\nmovie_all_frames off\ncamera cameras/rtmp.conf\n",
        fixtures / "noise-main.conf");
    assert(noise.global.camera_defaults.noise_level == 64);
    assert(!noise.global.camera_defaults.noise_tune);
    assert(!noise.global.camera_defaults.movie_all_frames);
    const std::string noise_dump = noise.dump_effective();
    assert(noise_dump.find("noise_tune off") != std::string::npos);
    assert(noise_dump.find("movie_all_frames off") != std::string::npos);

    expect_config_error([] { ConfigParser().parse_string("daemon maybe\n"); });
    expect_config_error([&] { load_config(fixtures / "invalid-main.conf"); });
    expect_config_error(
        [] { ConfigParser(ParseOptions{true, true}).parse_string("not_supported 1\n"); });

    std::cout << "config tests passed\n";
}
