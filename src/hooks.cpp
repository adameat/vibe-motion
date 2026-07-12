#include "vibe_motion/hooks.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vibe_motion {
namespace {

std::string token_value(char conversion, const HookTokens& tokens, bool& found) {
    const std::string short_key(1, conversion);
    auto iterator = tokens.find(short_key);
    if (iterator == tokens.end()) {
        iterator = tokens.find("%" + short_key);
    }
    found = iterator != tokens.end();
    return found ? iterator->second : std::string{};
}

} // namespace

std::vector<std::string> parse_hook_command(const std::string& command) {
    enum class Quote { none, single, double_quote };
    Quote quote = Quote::none;
    bool escaped = false;
    bool started = false;
    std::string current;
    std::vector<std::string> result;

    const auto finish_argument = [&] {
        if (started) {
            result.push_back(current);
            current.clear();
            started = false;
        }
    };
    for (const char character : command) {
        if (escaped) {
            current.push_back(character);
            started = true;
            escaped = false;
            continue;
        }
        if (character == '\\' && quote != Quote::single) {
            escaped = true;
            started = true;
            continue;
        }
        if (character == '\'' && quote != Quote::double_quote) {
            quote = quote == Quote::single ? Quote::none : Quote::single;
            started = true;
            continue;
        }
        if (character == '"' && quote != Quote::single) {
            quote = quote == Quote::double_quote ? Quote::none : Quote::double_quote;
            started = true;
            continue;
        }
        if ((character == ' ' || character == '\t' || character == '\r' || character == '\n') &&
            quote == Quote::none) {
            finish_argument();
            continue;
        }
        current.push_back(character);
        started = true;
    }
    if (escaped || quote != Quote::none) {
        throw std::invalid_argument("unterminated quote or escape in hook command");
    }
    finish_argument();
    if (result.empty() || result.front().empty()) {
        throw std::invalid_argument("hook command is empty");
    }
    return result;
}

std::string expand_hook_tokens(const std::string& value, const HookTokens& tokens) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%' || index + 1 >= value.size()) {
            result.push_back(value[index]);
            continue;
        }
        const char conversion = value[++index];
        if (conversion == '%') {
            result.push_back('%');
            continue;
        }
        bool found = false;
        const auto replacement = token_value(conversion, tokens, found);
        if (found) {
            result += replacement;
        } else {
            result.push_back('%');
            result.push_back(conversion);
        }
    }
    return result;
}

HookExecutor::HookExecutor(HookExecutorOptions options) : options_(std::move(options)) {
    if (options_.max_concurrent == 0 || options_.max_pending == 0 ||
        options_.timeout.count() <= 0 || options_.terminate_grace.count() < 0 ||
        options_.child_comm.empty()) {
        throw std::invalid_argument("invalid hook executor options");
    }
    worker_ = std::thread(&HookExecutor::run, this);
}

HookExecutor::~HookExecutor() {
    stop();
}

bool HookExecutor::submit(const std::string& command, const HookTokens& tokens,
                          HookCompletion completion) {
    auto argv = parse_hook_command(command);
    for (auto& argument : argv) {
        argument = expand_hook_tokens(argument, tokens);
    }
    return submit(std::move(argv), std::move(completion));
}

bool HookExecutor::submit(std::vector<std::string> argv, HookCompletion completion) {
    if (argv.empty() || argv.front().empty()) {
        throw std::invalid_argument("hook argv is empty");
    }
    for (const auto& argument : argv) {
        if (argument.find('\0') != std::string::npos) {
            throw std::invalid_argument("hook argument contains NUL");
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || jobs_.size() >= options_.max_pending) {
        return false;
    }
    jobs_.push_back({std::move(argv), std::move(completion)});
    wake_.notify_one();
    return true;
}

std::size_t HookExecutor::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

std::size_t HookExecutor::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return children_.size();
}

bool HookExecutor::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, timeout, [&] { return jobs_.empty() && children_.empty(); });
}

void HookExecutor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && !worker_.joinable()) {
            return;
        }
        stopping_ = true;
        jobs_.clear();
        for (auto& child : children_) {
            if (!child.timed_out) {
                (void)::kill(-child.pid, SIGTERM);
                child.timed_out = true;
                child.terminated = std::chrono::steady_clock::now();
            }
        }
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void HookExecutor::launch(Job job) {
    int error_pipe[2] = {-1, -1};
    if (::pipe2(error_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        HookResult result;
        result.argv = job.argv;
        result.exec_errno = errno;
        if (job.completion) {
            completions_.emplace_back(std::move(job.completion), std::move(result));
        }
        return;
    }

    std::vector<char*> raw_argv;
    raw_argv.reserve(job.argv.size() + 1);
    for (auto& argument : job.argv) {
        raw_argv.push_back(const_cast<char*>(argument.c_str()));
    }
    raw_argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(error_pipe[0]);
        (void)::setsid();
        (void)::prctl(PR_SET_NAME, options_.child_comm.c_str(), 0, 0, 0);

        // Keep this tiny supervisor alive as the hook's parent.  Some existing
        // Motion hook scripts identify the camera daemon through /proc/PPID/comm.
        const pid_t command_pid = ::fork();
        if (command_pid == 0) {
            ::execvp(raw_argv.front(), raw_argv.data());
            const int child_errno = errno;
            (void)::write(error_pipe[1], &child_errno, sizeof(child_errno));
            ::_exit(127);
        }
        if (command_pid < 0) {
            const int child_errno = errno;
            (void)::write(error_pipe[1], &child_errno, sizeof(child_errno));
            ::_exit(127);
        }
        ::close(error_pipe[1]);
        int command_status = 0;
        while (::waitpid(command_pid, &command_status, 0) < 0 && errno == EINTR) {
        }
        if (WIFEXITED(command_status)) {
            ::_exit(WEXITSTATUS(command_status));
        }
        if (WIFSIGNALED(command_status)) {
            ::_exit(128 + WTERMSIG(command_status));
        }
        ::_exit(127);
    }
    ::close(error_pipe[1]);
    if (pid < 0) {
        const int saved_errno = errno;
        ::close(error_pipe[0]);
        HookResult result;
        result.argv = job.argv;
        result.exec_errno = saved_errno;
        if (job.completion) {
            completions_.emplace_back(std::move(job.completion), std::move(result));
        }
        return;
    }
    children_.push_back(
        {pid, error_pipe[0], std::chrono::steady_clock::now(), {}, false, false, std::move(job)});
}

void HookExecutor::finish(std::size_t index, int status, int exec_error) {
    Child child = std::move(children_[index]);
    children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(index));
    if (child.error_fd >= 0) {
        ::close(child.error_fd);
    }
    HookResult result;
    result.argv = std::move(child.job.argv);
    result.process_id = static_cast<int>(child.pid);
    result.exec_errno = exec_error;
    result.timed_out = child.timed_out;
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.term_signal = WTERMSIG(status);
    }
    if (child.job.completion) {
        completions_.emplace_back(std::move(child.job.completion), std::move(result));
    }
}

void HookExecutor::reap_and_expire() {
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < children_.size();) {
        auto& child = children_[index];
        int status = 0;
        const pid_t waited = ::waitpid(child.pid, &status, WNOHANG);
        if (waited == child.pid) {
            int exec_error = 0;
            // error_fd is O_NONBLOCK; the analyzer cannot infer pipe2's flag.
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
            if (::read(child.error_fd, &exec_error, sizeof(exec_error)) < 0 && errno != EAGAIN) {
                exec_error = 0;
            }
            finish(index, status, exec_error);
            continue;
        }
        if (waited < 0 && errno == ECHILD) {
            finish(index, 0, 0);
            continue;
        }
        if (!child.timed_out && now - child.started >= options_.timeout) {
            (void)::kill(-child.pid, SIGTERM);
            child.timed_out = true;
            child.terminated = now;
        } else if (child.timed_out && !child.sent_kill &&
                   now - child.terminated >= options_.terminate_grace) {
            (void)::kill(-child.pid, SIGKILL);
            child.sent_kill = true;
        }
        ++index;
    }
}

void HookExecutor::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        while (!stopping_ && !jobs_.empty() && children_.size() < options_.max_concurrent) {
            Job job = std::move(jobs_.front());
            jobs_.pop_front();
            launch(std::move(job));
        }
        reap_and_expire();
        while (!completions_.empty()) {
            auto completion = std::move(completions_.front());
            completions_.pop_front();
            lock.unlock();
            try {
                completion.first(completion.second);
            } catch (...) {
                // User callbacks must not stop child reaping.
            }
            lock.lock();
        }
        if (jobs_.empty() && children_.empty()) {
            idle_.notify_all();
            if (stopping_) {
                break;
            }
        }
        wake_.wait_for(lock, std::chrono::milliseconds(25));
    }
}

} // namespace vibe_motion
