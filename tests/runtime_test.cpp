#include "runtime_util.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>

using namespace vibe_motion::runtime_detail;

int main() {
    assert(http_camera_url("http://camera.test/onvif/device_service"));
    assert(http_camera_url("HTTPS://camera.test/onvif/device_service"));
    assert(http_camera_url("HtTp://camera.test/onvif/device_service"));
    assert(!http_camera_url("rtsp://camera.test/stream"));

    assert(contains_case_insensitive("http://camera.test/OnViF/device_service", "/onvif/"));
    assert(contains_case_insensitive("Mixed Case Value", "CASE"));
    assert(!contains_case_insensitive("Mixed Case Value", "missing"));

    assert(onvif_identification_failure_is_fatal(true, "HTTP://camera.test/OnViF/device_service"));
    assert(!onvif_identification_failure_is_fatal(false, "rtsp://camera.test/onvif/main"));
    assert(!onvif_identification_failure_is_fatal(true, "https://camera.test/media/main"));

    assert(auto_baichuan_open_failure_requires_reselection("auto", true, false));
    assert(!auto_baichuan_open_failure_requires_reselection("auto", true, true));
    assert(!auto_baichuan_open_failure_requires_reselection("auto", false, false));
    assert(!auto_baichuan_open_failure_requires_reselection("baichuan", true, false));

    const auto instant = std::chrono::system_clock::time_point{std::chrono::seconds{1704196800}};
    const std::string hourly = timelapse_period_key(instant, "hourly");
    const std::string daily = timelapse_period_key(instant, " DAILY ");
    assert(hourly.size() == 10);
    assert(daily.size() == 8);
    assert(hourly.starts_with(daily));
    bool invalid_mode_thrown = false;
    try {
        static_cast<void>(timelapse_period_key(instant, "weekly"));
    } catch (const std::invalid_argument&) {
        invalid_mode_thrown = true;
    }
    assert(invalid_mode_thrown);
}
