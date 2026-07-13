#include "vibe_motion/hooks.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vibe_motion {
namespace {

constexpr std::uint32_t request_launch = 1;
constexpr std::uint32_t request_stop = 2;
constexpr std::uint32_t response_launched = 1;
constexpr std::uint32_t response_finished = 2;
// Keep each SOCK_SEQPACKET message comfortably below Linux's default Unix
// socket send-buffer limit. Motion hook command lines are normally tiny.
constexpr std::size_t maximum_request_size = 64 * 1024;

void require_single_threaded_process() {
    std::error_code error;
    std::filesystem::directory_iterator task("/proc/self/task", error);
    const std::filesystem::directory_iterator end;
    if (error) {
        throw std::runtime_error("cannot inspect process threads: " + error.message());
    }

    std::size_t task_count = 0;
    while (task != end) {
        if (++task_count > 1) {
            throw std::logic_error("HookExecutor must be constructed before other threads start");
        }
        task.increment(error);
        if (error) {
            throw std::runtime_error("cannot inspect process threads: " + error.message());
        }
    }
    if (task_count != 1) {
        throw std::runtime_error("cannot determine process thread count");
    }
}

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

bool send_packet(int socket, const void* data, std::size_t size) {
    for (;;) {
        const auto sent = ::send(socket, data, size, MSG_NOSIGNAL);
        if (sent == static_cast<ssize_t>(size)) {
            return true;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

void send_response(int socket, const SupervisorResponse& response) {
    (void)send_packet(socket, &response, sizeof(response));
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
                        send_response(socket,
                                      {response_finished, 0, header.job_id, -1, 127 << 8, EINVAL});
                        continue;
                    }

                    int error_pipe[2] = {-1, -1};
                    if (::pipe2(error_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
                        send_response(socket,
                                      {response_finished, 0, header.job_id, -1, 127 << 8, errno});
                        continue;
                    }
                    std::vector<char*> raw_arguments;
                    raw_arguments.reserve(arguments.size() + 1);
                    for (auto& argument : arguments) {
                        raw_arguments.push_back(argument.data());
                    }
                    raw_arguments.push_back(nullptr);

                    const pid_t pid = ::fork();
                    if (pid == 0) {
                        ::close(socket);
                        ::close(error_pipe[0]);
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
                        send_response(socket, {response_finished, 0, header.job_id, -1, 127 << 8,
                                               saved_errno});
                        continue;
                    }
                    children.push_back({header.job_id, pid, error_pipe[0]});
                    send_response(socket, {response_launched, 0, header.job_id, pid, 0, 0});
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
                send_response(socket, {response_finished, 0, children[index].job_id,
                                       children[index].pid, status, exec_error});
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
    require_single_threaded_process();
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        const int saved_errno = errno;
        throw std::runtime_error(std::string("cannot create hook supervisor socket: ") +
                                 std::strerror(saved_errno));
    }
    supervisor_pid_ = ::fork();
    if (supervisor_pid_ == 0) {
        ::close(sockets[0]);
        ::_exit(hook_supervisor(sockets[1], options_.child_comm));
    }
    ::close(sockets[1]);
    if (supervisor_pid_ < 0) {
        const int saved_errno = errno;
        ::close(sockets[0]);
        throw std::runtime_error(std::string("cannot start hook supervisor: ") +
                                 std::strerror(saved_errno));
    }
    supervisor_socket_ = sockets[0];
    const int socket_flags = ::fcntl(supervisor_socket_, F_GETFL);
    if (socket_flags < 0 || ::fcntl(supervisor_socket_, F_SETFL, socket_flags | O_NONBLOCK) != 0) {
        const int saved_errno = errno;
        stop_supervisor();
        throw std::runtime_error(std::string("cannot configure hook supervisor socket: ") +
                                 std::strerror(saved_errno));
    }
    try {
        worker_ = std::thread(&HookExecutor::run, this);
    } catch (...) {
        stop_supervisor();
        throw;
    }
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
        HookResult result;
        result.argv = job.argv;
        result.exec_errno = E2BIG;
        if (job.completion) {
            completions_.emplace_back(std::move(job.completion), std::move(result));
        }
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
    if (!send_packet(supervisor_socket_, request.data(), request.size())) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        const int send_error = errno == 0 ? EPIPE : errno;
        HookResult result;
        result.argv = job.argv;
        result.exec_errno = send_error;
        if (job.completion) {
            completions_.emplace_back(std::move(job.completion), std::move(result));
        }
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
    stopping_ = true;
    const int failure = error == 0 ? EPIPE : error;
    if (supervisor_socket_ >= 0) {
        ::close(supervisor_socket_);
        supervisor_socket_ = -1;
    }
    reap_supervisor_nonblocking();
    while (!children_.empty()) {
        if (children_.front().pid > 0) {
            (void)::kill(-children_.front().pid, SIGKILL);
        }
        finish(0, 127 << 8, failure);
    }
    while (!jobs_.empty()) {
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        if (job.completion) {
            HookResult result;
            result.argv = std::move(job.argv);
            result.exec_errno = failure;
            completions_.emplace_back(std::move(job.completion), std::move(result));
        }
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
        while (!stopping_ && !jobs_.empty() && children_.size() < options_.max_concurrent) {
            Job job = std::move(jobs_.front());
            jobs_.pop_front();
            if (!launch(job)) {
                jobs_.push_front(std::move(job));
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
        if (jobs_.empty() && children_.empty()) {
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
