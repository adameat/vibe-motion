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

    assert(camera_thread_name(12, "source") == "cam12-source");
    assert(camera_thread_name(12, "tl") == "cam12-tl");
    assert(camera_thread_name(123456, "long-role-name").size() == 15);

    assert(snapshot_phase_seconds(10, 15) == 10);
    assert(snapshot_phase_seconds(11, 15) == 14);
    assert(snapshot_phase_seconds(12, 15) == 3);
    assert(snapshot_phase_seconds(13, 15) == 7);
    assert(snapshot_phase_seconds(14, 15) == 11);
    assert(snapshot_phase_seconds(15, 15) == 0);
    assert(snapshot_phase_seconds(-1, 15) == 11);
    assert(snapshot_bucket_at(99, 15, 10) == 5);
    assert(snapshot_bucket_at(100, 15, 10) == 6);
    assert(snapshot_bucket_at(114, 15, 10) == 6);
    assert(snapshot_bucket_at(115, 15, 10) == 7);
    assert(snapshot_bucket_at(100, 15, 11) == 5);
    assert(snapshot_bucket_at(103, 15, 11) == 5);
    assert(snapshot_bucket_at(104, 15, 11) == 6);
    bool invalid_snapshot_interval_thrown = false;
    try {
        static_cast<void>(snapshot_phase_seconds(10, 0));
    } catch (const std::invalid_argument&) {
        invalid_snapshot_interval_thrown = true;
    }
    assert(invalid_snapshot_interval_thrown);

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
