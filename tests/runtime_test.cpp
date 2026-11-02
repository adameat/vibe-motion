#include "runtime_util.hpp"
#include "vibe_motion/event.hpp"

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

    assert(onvif_topic_holds_motion_state("tns1:RuleEngine/CellMotionDetector/Motion"));
    assert(onvif_topic_holds_motion_state("tns1:VideoSource/MotionAlarm"));
    assert(onvif_topic_holds_motion_state("Motion"));
    assert(!onvif_topic_holds_motion_state("tns1:RuleEngine/MyRuleDetector/PeopleDetect"));
    assert(!onvif_topic_holds_motion_state("tns1:RuleEngine/MyRuleDetector/VehicleDetect"));

    const auto state_start = std::chrono::steady_clock::time_point{};
    OnvifStateTracker states;
    states.update("motion", true, state_start);
    states.update("motion", true, state_start + std::chrono::minutes(59));
    assert(states.active());
    assert(states.expire(state_start + std::chrono::minutes(59), std::chrono::hours(1)).empty());
    const auto expired = states.expire(state_start + std::chrono::hours(1), std::chrono::hours(1));
    assert(expired.size() == 1 && expired.front() == "motion");
    assert(!states.active());

    states.update("motion", true, state_start);
    states.update("alarm", true, state_start + std::chrono::minutes(30));
    assert(states.expire(state_start + std::chrono::hours(1), std::chrono::hours(1)).size() == 1);
    assert(states.active());
    states.update("alarm", false, state_start + std::chrono::hours(1));
    assert(!states.active());

    // Renewable states use the last true, and false affects only its own key.
    OnvifStateTracker leases;
    const auto ttl = std::chrono::seconds(30);
    leases.update("motion", true, state_start, true);
    leases.update("motion", true, state_start + std::chrono::seconds(20), true);
    assert(leases.expire(state_start + ttl, ttl).empty());
    leases.update("pet", true, state_start + std::chrono::seconds(25), true);
    leases.update("motion", false, state_start + ttl, true);
    assert(leases.active());
    assert(leases.expire(state_start + std::chrono::seconds(54), ttl).empty());
    const auto expired_pet = leases.expire(state_start + std::chrono::seconds(55), ttl);
    assert(expired_pet.size() == 1 && expired_pet.front() == "pet");
    assert(!leases.active());
    assert(leases.expire(state_start + std::chrono::hours(1), ttl).empty());

    // Missing false: expire the state, then allow the normal 30-second tail.
    vibe_motion::EventStateMachine lease_events({.event_gap = std::chrono::seconds(30)});
    leases.update("motion", true, state_start, true);
    assert(lease_events.update(leases.active(), state_start).event_started);
    for (int second = 1; second <= 60; ++second) {
        const auto now = state_start + std::chrono::seconds(second);
        leases.expire(now, ttl);
        const auto decision = lease_events.update(leases.active(), now);
        // Last qualifying frame is at t=29, so the tail ends at t=59.
        assert(decision.event_ended == (second == 59));
        assert(!decision.event_started);
    }
    assert(!lease_events.active());
    // Silence cannot restart an expired event, but a new smart trigger can.
    leases.update("people", true, state_start + std::chrono::seconds(70), true);
    assert(
        lease_events.update(leases.active(), state_start + std::chrono::seconds(70)).event_started);
    leases.update("people", false, state_start + std::chrono::seconds(71), true);
    assert(
        !lease_events.update(leases.active(), state_start + std::chrono::seconds(71)).event_ended);
    assert(
        lease_events.update(leases.active(), state_start + std::chrono::seconds(100)).event_ended);

    // Repeated true can sustain a lease beyond the old one-hour hard limit.
    for (int second = 0; second <= 3700; second += 10) {
        const auto now = state_start + std::chrono::seconds(second);
        leases.update("motion", true, now, true);
        assert(leases.expire(now, ttl).empty());
        assert(leases.active());
    }
    leases.clear();
    assert(!leases.active());

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
