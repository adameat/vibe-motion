#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vibe_motion {

using HookTokens = std::unordered_map<std::string, std::string>;

// Parses the small, shell-like quoting language accepted in Motion hook options.
// No substitutions, globbing, redirections, pipelines, or shell operators exist.
std::vector<std::string> parse_hook_command(const std::string& command);

// Tokens can be supplied either as "%f" or "f".  Unknown conversions are kept.
// A literal percent is written as %%.
std::string expand_hook_tokens(const std::string& value, const HookTokens& tokens);

struct HookExecutorOptions {
    std::size_t max_concurrent = 4;
    std::size_t max_pending = 64;
    std::chrono::milliseconds timeout{300000};
    std::chrono::milliseconds terminate_grace{2000};
    std::string child_comm = "motion";
    std::string supervisor_program = "/proc/self/exe";
    std::chrono::milliseconds restart_delay{250};
    int supervisor_socket_buffer = 0;
    std::chrono::milliseconds supervisor_response_timeout{5000};
};

enum class HookPriority { normal, critical };

struct HookSubmitOptions {
    HookPriority priority = HookPriority::normal;
    std::string kind = "hook";
    int camera_id = 0;
    // Jobs with the same non-empty key never overlap. Queue priority is unchanged,
    // while jobs with other keys may bypass blocked work to use available concurrency.
    std::string serial_key;
    // Pending work with the same key is replaced by the newest submission.
    // Running jobs are never replaced.
    std::string coalesce_key;
};

struct HookResult {
    std::vector<std::string> argv;
    int process_id = -1;
    int exit_code = -1;
    int term_signal = 0;
    int exec_errno = 0;
    bool timed_out = false;
};

using HookCompletion = std::function<void(const HookResult&)>;

struct HookExecutorStatus {
    std::size_t pending = 0;
    std::size_t running = 0;
    std::size_t max_pending = 0;
    std::size_t max_concurrent = 0;
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t timed_out = 0;
    std::uint64_t failed = 0;
    std::uint64_t dropped = 0;
    std::uint64_t coalesced = 0;
    std::uint64_t backpressure = 0;
    std::uint64_t supervisor_restarts = 0;
    bool supervisor_healthy = false;
    int supervisor_pid = -1;
    int last_error = 0;
};

// Internal entry point used by the exec-spawned supervisor process.
int run_hook_supervisor(int socket, const std::string& process_name);

class HookExecutor {
  public:
    // The persistent hook supervisor is exec-spawned, so construction is safe
    // after other application threads have started.
    explicit HookExecutor(HookExecutorOptions options = {});
    ~HookExecutor();

    HookExecutor(const HookExecutor&) = delete;
    HookExecutor& operator=(const HookExecutor&) = delete;

    // Returns false when stopped or when the bounded pending queue is full.
    // Invalid command syntax throws std::invalid_argument.
    bool submit(const std::string& command, const HookTokens& tokens = {},
                HookCompletion completion = {});
    // Use this overload when a richer Motion/strftime expander is needed: parse
    // first, expand each argument independently, then submit the resulting argv.
    bool submit(std::vector<std::string> argv, HookCompletion completion = {});
    bool submit(std::vector<std::string> argv, HookSubmitOptions options,
                HookCompletion completion = {});
    bool wait_idle(std::chrono::milliseconds timeout);
    void stop();

    std::size_t pending() const;
    std::size_t running() const;
    HookExecutorStatus status() const;

  private:
    struct Job {
        std::vector<std::string> argv;
        HookSubmitOptions options;
        HookCompletion completion;
    };
    struct Child {
        std::uint64_t job_id = 0;
        pid_t pid = -1;
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point terminated;
        bool timed_out = false;
        bool sent_kill = false;
        Job job;
    };

    void run();
    bool start_supervisor();
    // Returns false when the supervisor socket applies backpressure; the caller
    // keeps the job queued and retries after a short wait.
    bool launch(Job& job);
    void receive_supervisor_results();
    void reap_and_expire();
    void finish(std::size_t index, int status, int exec_error);
    void fail_supervisor(int error);
    void reap_supervisor_nonblocking() noexcept;
    void stop_supervisor() noexcept;
    std::size_t pending_unlocked() const noexcept;
    std::optional<Job> pop_next_job();
    bool coalesce_pending(Job& replacement);
    bool evict_coalescible_job();
    void complete_job(Job job, int error);

    HookExecutorOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> critical_jobs_;
    std::deque<Job> jobs_;
    std::vector<Child> children_;
    std::deque<std::pair<HookCompletion, HookResult>> completions_;
    bool stopping_ = false;
    int supervisor_socket_ = -1;
    pid_t supervisor_pid_ = -1;
    std::uint64_t next_job_id_ = 1;
    bool supervisor_failed_ = false;
    std::chrono::steady_clock::time_point restart_after_{};
    std::uint64_t submitted_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t timed_out_ = 0;
    std::uint64_t failed_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t coalesced_ = 0;
    std::uint64_t backpressure_ = 0;
    std::uint64_t supervisor_restarts_ = 0;
    int last_error_ = 0;
    std::thread worker_;
};

} // namespace vibe_motion
