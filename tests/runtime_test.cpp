#include "runtime_util.hpp"

#include <cassert>

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
}
