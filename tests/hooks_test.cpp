#include "vibe_motion/hooks.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <utility>

using namespace vibe_motion;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

HookExecutorOptions options(const std::filesystem::path& supervisor, std::size_t max_concurrent,
                            std::size_t max_pending, std::chrono::milliseconds timeout = 5s,
                            std::chrono::milliseconds terminate_grace = 100ms,
                            int socket_buffer = 0) {
    HookExecutorOptions result;
    result.max_concurrent = max_concurrent;
    result.max_pending = max_pending;
    result.timeout = timeout;
    result.terminate_grace = terminate_grace;
    result.child_comm = "motion";
    result.supervisor_program = supervisor.string();
    result.restart_delay = 25ms;
    result.supervisor_socket_buffer = socket_buffer;
    result.supervisor_response_timeout = 1s;
    return result;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_parsing() {
    const auto parsed = parse_hook_command("/bin/tool one \"two words\" 'three words'");
    assert(parsed.size() == 4);
    assert(parsed[2] == "two words" && parsed[3] == "three words");
    assert(expand_hook_tokens("file=%f %% %x", {{"f", "a b"}}) == "file=a b % %x");
}

void test_persistent_named_supervisor(const std::filesystem::path& supervisor) {
    const auto output =
        std::filesystem::temp_directory_path() / ("vibe-motion-hook-" + std::to_string(::getpid()));
    const auto second_output = output.string() + "-second";
    std::ifstream original_name_input("/proc/self/comm");
    std::string original_process_name;
    original_name_input >> original_process_name;

    HookExecutor executor(options(supervisor, 1, 4));
    const std::string shell = "printf '%s ' \"$PPID\" > '" + output.string() +
                              "'; cat /proc/$PPID/comm >> '" + output.string() + "'";
    assert(executor.submit(std::vector<std::string>{"/bin/sh", "-c", shell}));
    assert(executor.wait_idle(5s));
    const std::string second_shell = "printf '%s ' \"$PPID\" > '" + second_output +
                                     "'; cat /proc/$PPID/comm >> '" + second_output + "'";
    assert(executor.submit(std::vector<std::string>{"/bin/sh", "-c", second_shell}));
    assert(executor.wait_idle(5s));

    std::ifstream input(output);
    std::ifstream second_input(second_output);
    pid_t parent_pid = -1;
    pid_t second_parent_pid = -1;
    std::string parent_name;
    std::string second_parent_name;
    input >> parent_pid >> parent_name;
    second_input >> second_parent_pid >> second_parent_name;
    std::filesystem::remove(output);
    std::filesystem::remove(second_output);
    assert(parent_pid > 0 && parent_pid == second_parent_pid);
    assert(parent_pid == executor.status().supervisor_pid);
    assert(parent_name == "motion" && second_parent_name == "motion");

    executor.stop();
    std::ifstream final_name_input("/proc/self/comm");
    std::string final_process_name;
    final_name_input >> final_process_name;
    assert(!original_process_name.empty() && final_process_name == original_process_name);
}

void test_queue_drains_after_saturation(const std::filesystem::path& supervisor) {
    HookExecutor executor(options(supervisor, 1, 3));
    assert(executor.submit(std::vector<std::string>{"/bin/sleep", "0.2"}));
    assert(wait_until([&] { return executor.running() == 1; }));
    for (int index = 0; index < 3; ++index) {
        assert(executor.submit(std::vector<std::string>{"/bin/true"}));
    }
    assert(!executor.submit(std::vector<std::string>{"/bin/true"}));
    assert(executor.status().dropped == 1);
    assert(executor.wait_idle(5s));
    assert(executor.submit(std::vector<std::string>{"/bin/true"}));
    assert(executor.wait_idle(5s));
    const auto status = executor.status();
    assert(status.pending == 0 && status.running == 0);
    assert(status.completed == 5 && status.dropped == 1);
}

void test_snapshot_coalescing_and_event_priority(const std::filesystem::path& supervisor) {
    const auto output = std::filesystem::temp_directory_path() /
                        ("vibe-motion-hook-priority-" + std::to_string(::getpid()));
    std::filesystem::remove(output);
    HookExecutor executor(options(supervisor, 1, 3));
    const auto append = [&](const std::string& value) {
        return std::vector<std::string>{"/bin/sh", "-c",
                                        "printf '" + value + "' >> '" + output.string() + "'"};
    };
    assert(executor.submit(std::vector<std::string>{
        "/bin/sh", "-c", "printf 'B' >> '" + output.string() + "'; sleep 0.2"}));
    assert(wait_until([&] { return executor.running() == 1; }));
    assert(executor.submit(append("O"),
                           {.kind = "snapshot", .camera_id = 1, .coalesce_key = "snapshot:1"}));
    assert(executor.submit(append("N"),
                           {.kind = "snapshot", .camera_id = 1, .coalesce_key = "snapshot:1"}));
    assert(executor.submit(append("2"),
                           {.kind = "snapshot", .camera_id = 2, .coalesce_key = "snapshot:2"}));
    assert(executor.submit(append("3"),
                           {.kind = "snapshot", .camera_id = 3, .coalesce_key = "snapshot:3"}));
    assert(executor.pending() == 3);
    assert(executor.submit(append("C"), {.priority = HookPriority::critical,
                                         .kind = "event-end",
                                         .camera_id = 1,
                                         .coalesce_key = {}}));
    assert(executor.wait_idle(5s));
    const auto contents = read_file(output);
    std::filesystem::remove(output);
    assert(contents.rfind("BC", 0) == 0);
    assert(contents.find('O') == std::string::npos);
    assert(contents.find('N') == std::string::npos);
    const auto status = executor.status();
    assert(status.coalesced == 1 && status.dropped == 1);
}

void test_coalescing_moves_replacement_to_priority_queue(const std::filesystem::path& supervisor) {
    const auto output = std::filesystem::temp_directory_path() /
                        ("vibe-motion-hook-reprioritize-" + std::to_string(::getpid()));
    std::filesystem::remove(output);
    HookExecutor executor(options(supervisor, 1, 4));
    const auto append = [&](const std::string& value) {
        return std::vector<std::string>{"/bin/sh", "-c",
                                        "printf '" + value + "' >> '" + output.string() + "'"};
    };
    assert(executor.submit(std::vector<std::string>{
        "/bin/sh", "-c", "printf 'B' >> '" + output.string() + "'; sleep 0.2"}));
    assert(wait_until([&] { return executor.running() == 1; }));
    assert(executor.submit(append("O")));
    assert(executor.submit(append("N"), {.coalesce_key = "reprioritize"}));
    assert(executor.submit(append("C"),
                           {.priority = HookPriority::critical, .coalesce_key = "reprioritize"}));
    assert(executor.wait_idle(5s));
    assert(read_file(output) == "BCO");
    std::filesystem::remove(output);
    assert(executor.status().coalesced == 1);
}

void test_timeout_and_forced_kill(const std::filesystem::path& supervisor) {
    HookResult completion;
    HookExecutor executor(options(supervisor, 1, 4, 100ms, 50ms));
    assert(executor.submit(std::vector<std::string>{"/bin/sh", "-c", "trap '' TERM; sleep 5"},
                           [&](const HookResult& result) { completion = result; }));
    assert(executor.wait_idle(5s));
    assert(completion.timed_out);
    assert(completion.term_signal == SIGKILL);
    const auto status = executor.status();
    assert(status.timed_out == 1 && status.failed == 1);
}

void test_exec_failure(const std::filesystem::path& supervisor) {
    HookResult completion;
    HookExecutor executor(options(supervisor, 1, 4));
    assert(executor.submit(std::vector<std::string>{"/definitely/not/a/program"},
                           [&](const HookResult& result) { completion = result; }));
    assert(executor.wait_idle(5s));
    assert(completion.exec_errno == ENOENT);
    assert(completion.exit_code == 127);
    assert(executor.status().failed == 1);
}

void test_supervisor_failure_recovers(const std::filesystem::path& supervisor) {
    const auto output = std::filesystem::temp_directory_path() /
                        ("vibe-motion-hook-recovery-" + std::to_string(::getpid()));
    std::filesystem::remove(output);
    HookExecutor executor(options(supervisor, 1, 8, 10s));
    assert(executor.submit(std::vector<std::string>{"/bin/sleep", "5"}));
    assert(wait_until([&] { return executor.running() == 1; }));
    assert(executor.submit(
        std::vector<std::string>{"/bin/sh", "-c", "printf recovered > '" + output.string() + "'"}));
    const int original_supervisor = executor.status().supervisor_pid;
    assert(original_supervisor > 0 && ::kill(original_supervisor, SIGKILL) == 0);
    assert(wait_until([&] {
        const auto status = executor.status();
        return status.supervisor_healthy && status.supervisor_restarts >= 1 &&
               status.supervisor_pid > 0 && status.supervisor_pid != original_supervisor;
    }));
    assert(executor.wait_idle(5s));
    assert(read_file(output) == "recovered");
    std::filesystem::remove(output);
    const auto status = executor.status();
    assert(status.failed >= 1 && status.supervisor_restarts == 1);
}

void test_wedged_supervisor_recovers(const std::filesystem::path& supervisor) {
    auto executor_options = options(supervisor, 1, 8, 10s);
    executor_options.supervisor_response_timeout = 100ms;
    HookExecutor executor(std::move(executor_options));
    const int original_supervisor = executor.status().supervisor_pid;
    assert(original_supervisor > 0 && ::kill(original_supervisor, SIGSTOP) == 0);
    assert(executor.submit(std::vector<std::string>{"/bin/true"}));
    assert(wait_until([&] {
        const auto status = executor.status();
        return status.supervisor_healthy && status.supervisor_restarts >= 1 &&
               status.supervisor_pid > 0 && status.supervisor_pid != original_supervisor;
    }));
    assert(executor.submit(std::vector<std::string>{"/bin/true"}));
    assert(executor.wait_idle(5s));
    const auto status = executor.status();
    assert(status.supervisor_restarts == 1 && status.last_error == ETIMEDOUT);
}

void test_socket_backpressure_recovers(const std::filesystem::path& supervisor) {
    assert(::setenv("VIBE_HOOK_SUPERVISOR_START_DELAY_MS", "250", 1) == 0);
    HookExecutor executor(options(supervisor, 64, 128, 5s, 100ms, 1024));
    assert(::unsetenv("VIBE_HOOK_SUPERVISOR_START_DELAY_MS") == 0);
    for (int index = 0; index < 96; ++index) {
        assert(executor.submit(std::vector<std::string>{"/bin/true"}));
    }
    assert(executor.wait_idle(10s));
    const auto status = executor.status();
    assert(status.completed == 96 && status.failed == 0);
    assert(status.backpressure > 0);
}

void test_clean_shutdown(const std::filesystem::path& supervisor) {
    HookExecutor executor(options(supervisor, 1, 4));
    assert(executor.submit(std::vector<std::string>{"/bin/sleep", "5"}));
    assert(wait_until([&] { return executor.running() == 1; }));
    executor.stop();
}

void test_construction_with_other_threads(const std::filesystem::path& supervisor) {
    std::atomic<bool> running = true;
    std::thread other([&] {
        while (running.load()) {
            std::this_thread::yield();
        }
    });
    HookExecutor executor(options(supervisor, 1, 4));
    assert(executor.submit(std::vector<std::string>{"/bin/true"}));
    assert(executor.wait_idle(5s));
    running = false;
    other.join();
}

void test_construction_with_low_fd_limit(const std::filesystem::path& supervisor) {
    rlimit original_limit{};
    assert(::getrlimit(RLIMIT_NOFILE, &original_limit) == 0);
    rlimit limited = original_limit;
    limited.rlim_cur = original_limit.rlim_max == RLIM_INFINITY || original_limit.rlim_max >= 64
                           ? 64
                           : original_limit.rlim_max;
    assert(limited.rlim_cur > 3);
    assert(::setrlimit(RLIMIT_NOFILE, &limited) == 0);
    {
        HookExecutor executor(options(supervisor, 1, 4));
        assert(executor.submit(std::vector<std::string>{"/bin/true"}));
        assert(executor.wait_idle(5s));
    }
    assert(::setrlimit(RLIMIT_NOFILE, &original_limit) == 0);
}

} // namespace

int main(int argc, char** argv) {
    assert(argc > 0);
    const auto supervisor = std::filesystem::path(argv[0]).parent_path() / "vibe-motion";
    assert(std::filesystem::exists(supervisor));

    test_parsing();
    test_persistent_named_supervisor(supervisor);
    test_queue_drains_after_saturation(supervisor);
    test_snapshot_coalescing_and_event_priority(supervisor);
    test_coalescing_moves_replacement_to_priority_queue(supervisor);
    test_timeout_and_forced_kill(supervisor);
    test_exec_failure(supervisor);
    test_supervisor_failure_recovers(supervisor);
    test_wedged_supervisor_recovers(supervisor);
    test_socket_backpressure_recovers(supervisor);
    test_clean_shutdown(supervisor);
    test_construction_with_other_threads(supervisor);
    test_construction_with_low_fd_limit(supervisor);

    std::cout << "hook tests passed\n";
}
