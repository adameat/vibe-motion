#include "runtime_util.hpp"
#include "vibe_motion/event.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

using namespace vibe_motion::runtime_detail;

int main() {
    // Isolate the default SIGPIPE disposition so a regression kills only the
    // test child. Closing the reader reproduces a worker restart after open.
    const pid_t wake_child = ::fork();
    assert(wake_child >= 0);
    if (wake_child == 0) {
        std::signal(SIGPIPE, SIG_DFL);
        sigset_t pipe_signal, original, pending;
        ::sigemptyset(&pipe_signal);
        ::sigaddset(&pipe_signal, SIGPIPE);
        assert(::pthread_sigmask(SIG_UNBLOCK, &pipe_signal, nullptr) == 0);
        int descriptors[2];
        assert(::pipe(descriptors) == 0);
        assert(write_worker_wakeup(descriptors[1]));
        char byte = 0;
        assert(::read(descriptors[0], &byte, 1) == 1 && byte == 'j');
        ::close(descriptors[0]);
        assert(!write_worker_wakeup(descriptors[1]));
        assert(::pthread_sigmask(SIG_BLOCK, &pipe_signal, &original) == 0);
        assert(::sigismember(&original, SIGPIPE) == 0);
        assert(::sigpending(&pending) == 0);
        assert(::sigismember(&pending, SIGPIPE) == 0);
        // Do not swallow a SIGPIPE that was pending before the attempted write.
        assert(::raise(SIGPIPE) == 0);
        assert(!write_worker_wakeup(descriptors[1]));
        assert(::sigpending(&pending) == 0);
        assert(::sigismember(&pending, SIGPIPE) == 1);
        const timespec no_wait{0, 0};
        assert(::sigtimedwait(&pipe_signal, nullptr, &no_wait) == SIGPIPE);
        assert(::pthread_sigmask(SIG_SETMASK, &original, nullptr) == 0);
        struct sigaction disposition{};
        assert(::sigaction(SIGPIPE, nullptr, &disposition) == 0);
        assert(disposition.sa_handler == SIG_DFL);
        ::close(descriptors[1]);
        ::_exit(0);
    }
    int wake_status = 0;
    assert(::waitpid(wake_child, &wake_status, 0) == wake_child);
    assert(WIFEXITED(wake_status) && WEXITSTATUS(wake_status) == 0);

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

    // A hard-expired true is not a new edge, even if repeated indefinitely.
    assert(!states.update("motion", true, state_start + std::chrono::hours(2)));
    assert(!states.active());
    assert(states.expire(state_start + std::chrono::hours(3), std::chrono::hours(1)).empty());
    assert(states.update("motion", false, state_start + std::chrono::hours(3)));
    assert(states.update("motion", true, state_start + std::chrono::hours(3)));
    assert(states.active());
    states.clear();

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
    // Unlike hard-expired states, a renewable state accepts the same key again
    // without needing the missing false that this workaround is designed for.
    assert(leases.update("motion", true, state_start + std::chrono::seconds(61), true));
    assert(leases.active());
    leases.update("motion", false, state_start + std::chrono::seconds(62), true);
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
    // Clearing a disconnected subscription also clears old tombstones.
    states.clear();
    assert(states.update("motion", true, state_start));
    assert(states.active());

    char temporary[] = "/tmp/vibe-runtime-test-XXXXXX";
    const char* created = ::mkdtemp(temporary);
    assert(created != nullptr);
    const std::filesystem::path directory(created);
    const auto requested = directory / "daily.mkv";
    assert(unused_recording_path(requested) == requested);
    std::ofstream(requested) << "original";
    const auto first_part = unused_recording_path(requested);
    assert(first_part.filename() == "daily-part1.mkv");
    std::filesystem::create_symlink("missing", first_part);
    assert(unused_recording_path(requested).filename() == "daily-part2.mkv");
    std::filesystem::remove_all(directory);

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
