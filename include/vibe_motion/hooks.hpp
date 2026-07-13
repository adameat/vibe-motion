#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
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

class HookExecutor {
  public:
    // Construct while the process is single-threaded. The persistent hook
    // supervisor is initialized directly in the forked child.
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
    bool wait_idle(std::chrono::milliseconds timeout);
    void stop();

    std::size_t pending() const;
    std::size_t running() const;

  private:
    struct Job {
        std::vector<std::string> argv;
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
    // Returns false when the supervisor socket applies backpressure; the caller
    // keeps the job queued and retries after a short wait.
    bool launch(Job& job);
    void receive_supervisor_results();
    void reap_and_expire();
    void finish(std::size_t index, int status, int exec_error);
    void fail_supervisor(int error);
    void reap_supervisor_nonblocking() noexcept;
    void stop_supervisor() noexcept;

    HookExecutorOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    std::vector<Child> children_;
    std::deque<std::pair<HookCompletion, HookResult>> completions_;
    bool stopping_ = false;
    int supervisor_socket_ = -1;
    pid_t supervisor_pid_ = -1;
    std::uint64_t next_job_id_ = 1;
    bool supervisor_failed_ = false;
    std::thread worker_;
};

} // namespace vibe_motion
