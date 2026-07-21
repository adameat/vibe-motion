#include "vibe_motion/hooks.hpp"

#include "vibe_motion/log.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace vibe_motion {
namespace {

constexpr std::uint32_t request_launch = 1;
constexpr std::uint32_t request_stop = 2;
constexpr std::uint32_t response_launched = 1;
constexpr std::uint32_t response_finished = 2;
// Keep each SOCK_SEQPACKET message comfortably below Linux's default Unix
// socket send-buffer limit. Motion hook command lines are normally tiny.
constexpr std::size_t maximum_request_size = std::size_t{64} * 1024;

struct RequestHeader {
    std::uint32_t type = 0;
    std::uint32_t argument_count = 0;
    std::uint64_t job_id = 0;
};

struct SupervisorResponse {
    std::uint32_t type = 0;
    std::uint32_t reserved = 0;
    std::uint64_t job_id = 0;
    pid_t pid = -1;
    int status = 0;
    int exec_errno = 0;
};

struct SupervisorChild {
    std::uint64_t job_id = 0;
    pid_t pid = -1;
    int error_fd = -1;
};

bool send_packet(int socket, const void* data, std::size_t size, int* error = nullptr) {
    for (;;) {
        const auto sent = ::send(socket, data, size, MSG_NOSIGNAL);
        if (sent == static_cast<ssize_t>(size)) {
            if (error != nullptr) {
                *error = 0;
            }
            return true;
        }
        const int send_error = sent < 0 ? errno : EIO;
        if (send_error == EINTR) {
            continue;
        }
        if (error != nullptr) {
            *error = send_error;
        }
        return false;
    }
}

bool send_response(int socket, const SupervisorResponse& response) {
    return send_packet(socket, &response, sizeof(response));
}

int hook_supervisor(int socket, const std::string& process_name) {
    (void)::prctl(PR_SET_NAME, process_name.c_str(), 0, 0, 0);
    std::vector<SupervisorChild> children;
    std::vector<char> request(maximum_request_size);
    bool stopping = false;

    while (!stopping || !children.empty()) {
        pollfd descriptor{socket, POLLIN, 0};
        const int timeout = !stopping && children.empty() ? -1 : 25;
        int polled = 0;
        do {
            polled = ::poll(&descriptor, 1, timeout);
        } while (polled < 0 && errno == EINTR);

        if (polled > 0 && (descriptor.revents & POLLIN) != 0) {
            const auto received = ::recv(socket, request.data(), request.size(), 0);
            if (received <= 0) {
                stopping = true;
            } else if (static_cast<std::size_t>(received) >= sizeof(RequestHeader)) {
                RequestHeader header{};
                std::memcpy(&header, request.data(), sizeof(header));
                if (header.type == request_stop) {
                    stopping = true;
                } else if (header.type == request_launch && !stopping) {
                    std::size_t offset = sizeof(header);
                    std::vector<std::string> arguments;
                    bool valid = header.argument_count > 0 &&
                                 header.argument_count <=
                                     (static_cast<std::size_t>(received) - sizeof(header)) /
                                         sizeof(std::uint32_t);
                    if (valid) {
                        arguments.reserve(header.argument_count);
                    }
                    for (std::uint32_t index = 0; valid && index < header.argument_count; ++index) {
                        if (offset + sizeof(std::uint32_t) > static_cast<std::size_t>(received)) {
                            valid = false;
                            break;
                        }
                        std::uint32_t length = 0;
                        std::memcpy(&length, request.data() + offset, sizeof(length));
                        offset += sizeof(length);
                        if (offset + length > static_cast<std::size_t>(received)) {
                            valid = false;
                            break;
                        }
                        arguments.emplace_back(request.data() + offset, length);
                        offset += length;
                    }
                    valid = valid && offset == static_cast<std::size_t>(received) &&
                            !arguments.front().empty();
                    if (!valid) {
                        if (!send_response(socket, {response_finished, 0, header.job_id, -1,
                                                    127 << 8, EINVAL})) {
                            stopping = true;
                        }
                        continue;
                    }

                    int error_pipe[2] = {-1, -1};
                    if (::pipe2(error_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
                        if (!send_response(socket, {response_finished, 0, header.job_id, -1,
                                                    127 << 8, errno})) {
                            stopping = true;
                        }
                        continue;
                    }
                    std::vector<char*> raw_arguments;
                    raw_arguments.reserve(arguments.size() + 1);
                    for (auto& argument : arguments) {
                        raw_arguments.push_back(argument.data());
                    }
                    raw_arguments.push_back(nullptr);

                    const pid_t supervisor_pid = ::getpid();
                    const pid_t pid = ::fork();
                    if (pid == 0) {
                        ::close(socket);
                        ::close(error_pipe[0]);
                        (void)::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
                        if (::getppid() != supervisor_pid) {
                            ::_exit(127);
                        }
                        (void)::setsid();
                        ::execvp(raw_arguments.front(), raw_arguments.data());
                        const int child_errno = errno;
                        (void)::write(error_pipe[1], &child_errno, sizeof(child_errno));
                        ::_exit(127);
                    }
                    ::close(error_pipe[1]);
                    if (pid < 0) {
                        const int saved_errno = errno;
                        ::close(error_pipe[0]);
                        if (!send_response(socket, {response_finished, 0, header.job_id, -1,
                                                    127 << 8, saved_errno})) {
                            stopping = true;
                        }
                        continue;
                    }
                    children.push_back({header.job_id, pid, error_pipe[0]});
                    if (!send_response(socket, {response_launched, 0, header.job_id, pid, 0, 0})) {
                        stopping = true;
                    }
                }
            }
        } else if (polled > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            stopping = true;
        }

        for (std::size_t index = 0; index < children.size();) {
            int status = 0;
            const pid_t waited = ::waitpid(children[index].pid, &status, WNOHANG);
            if (waited == children[index].pid || (waited < 0 && errno == ECHILD)) {
                int exec_error = 0;
                if (::read(children[index].error_fd, &exec_error, sizeof(exec_error)) < 0 &&
                    errno != EAGAIN) {
                    exec_error = 0;
                }
                ::close(children[index].error_fd);
                if (!send_response(socket, {response_finished, 0, children[index].job_id,
                                            children[index].pid, status, exec_error})) {
                    stopping = true;
                }
                children.erase(children.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            if (stopping) {
                (void)::kill(-children[index].pid, SIGKILL);
            }
            ++index;
        }
    }
    ::close(socket);
    return 0;
}

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

int run_hook_supervisor(int socket, const std::string& process_name) {
    return hook_supervisor(socket, process_name);
}

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
        options_.child_comm.empty() || options_.supervisor_program.empty() ||
        options_.restart_delay.count() < 0 || options_.supervisor_socket_buffer < 0 ||
        options_.supervisor_response_timeout.count() <= 0) {
        throw std::invalid_argument("invalid hook executor options");
    }
    if (!start_supervisor()) {
        throw std::runtime_error(std::string("cannot start hook supervisor: ") +
                                 std::strerror(last_error_));
    }
    try {
        worker_ = std::thread(&HookExecutor::run, this);
    } catch (...) {
        stop_supervisor();
        throw;
    }
}

bool HookExecutor::start_supervisor() {
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        last_error_ = errno;
        return false;
    }
    if (options_.supervisor_socket_buffer > 0) {
        const int size = options_.supervisor_socket_buffer;
        for (const int socket : sockets) {
            (void)::setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
            (void)::setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
        }
    }

    // Keep the inherited descriptor below even restrictive RLIMIT_NOFILE values.
    constexpr int child_socket = 3;
    posix_spawn_file_actions_t actions;
    int spawn_error = ::posix_spawn_file_actions_init(&actions);
    const bool actions_initialized = spawn_error == 0;
    if (spawn_error == 0) {
        spawn_error = ::posix_spawn_file_actions_adddup2(&actions, sockets[1], child_socket);
    }
    if (spawn_error == 0 && sockets[0] != child_socket) {
        spawn_error = ::posix_spawn_file_actions_addclose(&actions, sockets[0]);
    }
    if (spawn_error == 0 && sockets[1] != child_socket) {
        spawn_error = ::posix_spawn_file_actions_addclose(&actions, sockets[1]);
    }
    std::string socket_argument = std::to_string(child_socket);
    std::vector<char*> arguments{options_.supervisor_program.data(),
                                 const_cast<char*>("--hook-supervisor"), socket_argument.data(),
                                 options_.child_comm.data(), nullptr};
    pid_t spawned_pid = -1;
    if (spawn_error == 0) {
        spawn_error = ::posix_spawn(&spawned_pid, options_.supervisor_program.c_str(), &actions,
                                    nullptr, arguments.data(), environ);
    }
    if (actions_initialized) {
        (void)::posix_spawn_file_actions_destroy(&actions);
    }
    ::close(sockets[1]);
    if (spawn_error != 0) {
        ::close(sockets[0]);
        last_error_ = spawn_error;
        return false;
    }
    const int socket_flags = ::fcntl(sockets[0], F_GETFL);
    if (socket_flags < 0 || ::fcntl(sockets[0], F_SETFL, socket_flags | O_NONBLOCK) != 0) {
        const int saved_errno = errno;
        ::close(sockets[0]);
        (void)::kill(spawned_pid, SIGKILL);
        int status = 0;
        while (::waitpid(spawned_pid, &status, 0) < 0 && errno == EINTR) {
        }
        last_error_ = saved_errno;
        return false;
    }
    supervisor_socket_ = sockets[0];
    supervisor_pid_ = spawned_pid;
    if (supervisor_failed_) {
        ++supervisor_restarts_;
    }
    supervisor_failed_ = false;
    return true;
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
    return submit(std::move(argv), HookSubmitOptions{}, std::move(completion));
}

bool HookExecutor::submit(std::vector<std::string> argv, HookSubmitOptions options,
                          HookCompletion completion) {
    if (argv.empty() || argv.front().empty()) {
        throw std::invalid_argument("hook argv is empty");
    }
    for (const auto& argument : argv) {
        if (argument.find('\0') != std::string::npos) {
            throw std::invalid_argument("hook argument contains NUL");
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++submitted_;
    if (stopping_) {
        ++dropped_;
        return false;
    }
    Job job{std::move(argv), std::move(options), std::move(completion)};
    if (coalesce_pending(job)) {
        wake_.notify_one();
        return true;
    }
    if (pending_unlocked() >= options_.max_pending &&
        (job.options.priority != HookPriority::critical || !evict_coalescible_job())) {
        ++dropped_;
        return false;
    }
    if (job.options.priority == HookPriority::critical) {
        critical_jobs_.push_back(std::move(job));
    } else {
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
    return true;
}

std::size_t HookExecutor::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_unlocked();
}

std::size_t HookExecutor::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return children_.size();
}

HookExecutorStatus HookExecutor::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {.pending = pending_unlocked(),
            .running = children_.size(),
            .max_pending = options_.max_pending,
            .max_concurrent = options_.max_concurrent,
            .submitted = submitted_,
            .completed = completed_,
            .timed_out = timed_out_,
            .failed = failed_,
            .dropped = dropped_,
            .coalesced = coalesced_,
            .backpressure = backpressure_,
            .supervisor_restarts = supervisor_restarts_,
            .supervisor_healthy =
                !supervisor_failed_ && supervisor_socket_ >= 0 && supervisor_pid_ > 0,
            .supervisor_pid = static_cast<int>(supervisor_pid_),
            .last_error = last_error_};
}

bool HookExecutor::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idle_.wait_for(lock, timeout,
                          [&] { return pending_unlocked() == 0 && children_.empty(); });
}

void HookExecutor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && !worker_.joinable()) {
            return;
        }
        stopping_ = true;
        dropped_ += pending_unlocked();
        critical_jobs_.clear();
        jobs_.clear();
        for (auto& child : children_) {
            if (child.pid > 0 && !child.timed_out) {
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

std::size_t HookExecutor::pending_unlocked() const noexcept {
    return critical_jobs_.size() + jobs_.size();
}

HookExecutor::Job HookExecutor::pop_next_job() {
    std::deque<Job>& queue = critical_jobs_.empty() ? jobs_ : critical_jobs_;
    Job job = std::move(queue.front());
    queue.pop_front();
    return job;
}

bool HookExecutor::coalesce_pending(Job& replacement) {
    if (replacement.options.coalesce_key.empty()) {
        return false;
    }
    const auto replace_in = [&](std::deque<Job>& queue) {
        const auto found = std::find_if(queue.begin(), queue.end(), [&](const Job& existing) {
            return existing.options.coalesce_key == replacement.options.coalesce_key;
        });
        if (found == queue.end()) {
            return false;
        }
        if (found->completion) {
            HookResult result;
            result.argv = std::move(found->argv);
            result.exec_errno = ECANCELED;
            completions_.emplace_back(std::move(found->completion), std::move(result));
        }
        queue.erase(found);
        std::deque<Job>& replacement_queue =
            replacement.options.priority == HookPriority::critical ? critical_jobs_ : jobs_;
        replacement_queue.push_back(std::move(replacement));
        ++coalesced_;
        return true;
    };
    return replace_in(critical_jobs_) || replace_in(jobs_);
}

bool HookExecutor::evict_coalescible_job() {
    const auto found = std::find_if(jobs_.begin(), jobs_.end(), [](const Job& job) {
        return !job.options.coalesce_key.empty();
    });
    if (found == jobs_.end()) {
        return false;
    }
    if (found->completion) {
        HookResult result;
        result.argv = std::move(found->argv);
        result.exec_errno = ECANCELED;
        completions_.emplace_back(std::move(found->completion), std::move(result));
    }
    jobs_.erase(found);
    ++dropped_;
    return true;
}

void HookExecutor::complete_job(Job job, int error) {
    ++completed_;
    if (error != 0) {
        ++failed_;
    }
    if (job.completion) {
        HookResult result;
        result.argv = std::move(job.argv);
        result.exec_errno = error;
        completions_.emplace_back(std::move(job.completion), std::move(result));
    }
}

bool HookExecutor::launch(Job& job) {
    const std::uint64_t job_id = next_job_id_;
    std::size_t request_size = sizeof(RequestHeader);
    bool request_too_large = false;
    for (const auto& argument : job.argv) {
        if (request_size > maximum_request_size - sizeof(std::uint32_t) ||
            argument.size() > maximum_request_size - request_size - sizeof(std::uint32_t)) {
            request_too_large = true;
            break;
        }
        request_size += sizeof(std::uint32_t) + argument.size();
    }
    if (request_too_large) {
        complete_job(std::move(job), E2BIG);
        return true;
    }

    std::vector<char> request(request_size);
    const RequestHeader header{request_launch, static_cast<std::uint32_t>(job.argv.size()), job_id};
    std::memcpy(request.data(), &header, sizeof(header));
    std::size_t offset = sizeof(header);
    for (const auto& argument : job.argv) {
        const auto length = static_cast<std::uint32_t>(argument.size());
        std::memcpy(request.data() + offset, &length, sizeof(length));
        offset += sizeof(length);
        std::memcpy(request.data() + offset, argument.data(), argument.size());
        offset += argument.size();
    }
    int send_error = 0;
    if (!send_packet(supervisor_socket_, request.data(), request.size(), &send_error)) {
        if (send_error == EAGAIN || send_error == EWOULDBLOCK) {
            ++backpressure_;
            return false;
        }
        send_error = send_error == 0 ? EPIPE : send_error;
        complete_job(std::move(job), send_error);
        fail_supervisor(send_error);
        return true;
    }
    ++next_job_id_;
    children_.push_back(
        {job_id, -1, std::chrono::steady_clock::now(), {}, false, false, std::move(job)});
    return true;
}

void HookExecutor::finish(std::size_t index, int status, int exec_error) {
    Child child = std::move(children_[index]);
    children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(index));
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
    ++completed_;
    if (result.timed_out) {
        ++timed_out_;
    }
    if (result.exec_errno != 0 || result.exit_code != 0 || result.term_signal != 0) {
        ++failed_;
    }
    if (child.job.completion) {
        completions_.emplace_back(std::move(child.job.completion), std::move(result));
    }
}

void HookExecutor::receive_supervisor_results() {
    if (supervisor_socket_ < 0) {
        return;
    }
    for (;;) {
        SupervisorResponse response{};
        // The socket is nonblocking and MSG_DONTWAIT is explicit.
        // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
        const auto received = ::recv(supervisor_socket_, &response, sizeof(response), MSG_DONTWAIT);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received != static_cast<ssize_t>(sizeof(response))) {
            fail_supervisor(received < 0 ? errno : EPIPE);
            break;
        }
        const auto found =
            std::find_if(children_.begin(), children_.end(),
                         [&](const Child& child) { return child.job_id == response.job_id; });
        if (found == children_.end()) {
            continue;
        }
        const auto index = static_cast<std::size_t>(std::distance(children_.begin(), found));
        if (response.type == response_launched) {
            found->pid = response.pid;
        } else if (response.type == response_finished) {
            if (found->pid < 0) {
                found->pid = response.pid;
            }
            finish(index, response.status, response.exec_errno);
        }
    }
}

void HookExecutor::fail_supervisor(int error) {
    if (supervisor_failed_) {
        return;
    }
    supervisor_failed_ = true;
    const int failure = error == 0 ? EPIPE : error;
    last_error_ = failure;
    if (supervisor_socket_ >= 0) {
        ::close(supervisor_socket_);
        supervisor_socket_ = -1;
    }
    if (supervisor_pid_ > 0) {
        (void)::kill(supervisor_pid_, SIGKILL);
    }
    reap_supervisor_nonblocking();
    while (!children_.empty()) {
        if (children_.front().pid > 0) {
            (void)::kill(-children_.front().pid, SIGKILL);
        }
        finish(0, 127 << 8, failure);
    }
    if (!stopping_) {
        restart_after_ = std::chrono::steady_clock::now() + options_.restart_delay;
        Logger::instance().write(LogLevel::error,
                                 "hook supervisor failed: ", std::strerror(failure),
                                 "; recovery scheduled");
    }
}

void HookExecutor::reap_and_expire() {
    receive_supervisor_results();
    if (supervisor_failed_) {
        reap_supervisor_nonblocking();
    }
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < children_.size();) {
        auto& child = children_[index];
        if (child.pid < 0 && now - child.started >= options_.supervisor_response_timeout) {
            fail_supervisor(ETIMEDOUT);
            return;
        }
        if (child.pid > 0 && !child.timed_out &&
            (stopping_ || now - child.started >= options_.timeout)) {
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

void HookExecutor::reap_supervisor_nonblocking() noexcept {
    if (supervisor_pid_ <= 0) {
        return;
    }
    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(supervisor_pid_, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == supervisor_pid_ || (waited < 0 && errno == ECHILD)) {
        supervisor_pid_ = -1;
    }
}

void HookExecutor::stop_supervisor() noexcept {
    if (supervisor_socket_ >= 0) {
        const RequestHeader stop{request_stop, 0, 0};
        (void)send_packet(supervisor_socket_, &stop, sizeof(stop));
        ::close(supervisor_socket_);
        supervisor_socket_ = -1;
    }
    if (supervisor_pid_ > 0) {
        int status = 0;
        while (::waitpid(supervisor_pid_, &status, 0) < 0 && errno == EINTR) {
        }
        supervisor_pid_ = -1;
    }
}

void HookExecutor::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (!stopping_ && supervisor_failed_ && supervisor_pid_ <= 0 &&
            std::chrono::steady_clock::now() >= restart_after_) {
            if (start_supervisor()) {
                Logger::instance().write(LogLevel::info, "hook supervisor restarted");
            } else {
                restart_after_ = std::chrono::steady_clock::now() + options_.restart_delay;
            }
        }
        while (!stopping_ && !supervisor_failed_ && pending_unlocked() > 0 &&
               children_.size() < options_.max_concurrent) {
            Job job = pop_next_job();
            if (!launch(job)) {
                if (job.options.priority == HookPriority::critical) {
                    critical_jobs_.push_front(std::move(job));
                } else {
                    jobs_.push_front(std::move(job));
                }
                break;
            }
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
        if (pending_unlocked() == 0 && children_.empty()) {
            idle_.notify_all();
            if (stopping_) {
                break;
            }
        }
        wake_.wait_for(lock, std::chrono::milliseconds(25));
    }
    lock.unlock();
    stop_supervisor();
}

} // namespace vibe_motion
