#include "vibe_motion/http.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace vibe_motion {
namespace {

constexpr const char* boundary = "vibe-motion-boundary";

std::string html_escape(const std::string& input) {
    std::string output;
    for (const char character : input) {
        switch (character) {
        case '&':
            output += "&amp;";
            break;
        case '<':
            output += "&lt;";
            break;
        case '>':
            output += "&gt;";
            break;
        case '"':
            output += "&quot;";
            break;
        case '\'':
            output += "&#39;";
            break;
        default:
            output.push_back(character);
            break;
        }
    }
    return output;
}

std::string url_escape(const std::string& input) {
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (const char raw_character : input) {
        const auto character = static_cast<unsigned char>(raw_character);
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' ||
            character == '.') {
            output << static_cast<char>(character);
        } else {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned>(character);
        }
    }
    return output.str();
}

std::string http_date(std::chrono::system_clock::time_point time) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm value{};
    ::gmtime_r(&raw, &value);
    char buffer[64]{};
    (void)::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &value);
    return buffer;
}

} // namespace

struct HttpServer::Client {
    explicit Client(int socket) : fd(socket) {}
    int fd;
    std::thread thread;
    std::atomic<bool> done{false};
};

HttpServer::HttpServer(HttpServerOptions options, StatusProvider status_provider)
    : options_(std::move(options)), status_provider_(std::move(status_provider)) {
    if (options_.bind_address.empty() || options_.max_clients == 0 ||
        options_.write_timeout.count() <= 0) {
        throw std::invalid_argument("invalid HTTP server options");
    }
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    if (running_.load()) {
        return;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(options_.port);
    const int lookup =
        ::getaddrinfo(options_.bind_address == "*" ? nullptr : options_.bind_address.c_str(),
                      service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        throw std::runtime_error(std::string("cannot resolve HTTP bind address: ") +
                                 ::gai_strerror(lookup));
    }
    int saved_errno = EADDRNOTAVAIL;
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int candidate =
            ::socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC, address->ai_protocol);
        if (candidate < 0) {
            saved_errno = errno;
            continue;
        }
        const int enabled = 1;
        (void)::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (::bind(candidate, address->ai_addr, address->ai_addrlen) == 0 &&
            ::listen(candidate, 32) == 0) {
            listener_ = candidate;
            break;
        }
        saved_errno = errno;
        ::close(candidate);
    }
    ::freeaddrinfo(addresses);
    if (listener_ < 0) {
        throw std::runtime_error(std::string("cannot bind HTTP listener: ") +
                                 std::strerror(saved_errno));
    }
    sockaddr_storage local{};
    socklen_t local_size = sizeof(local);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&local), &local_size) == 0) {
        if (local.ss_family == AF_INET) {
            bound_port_.store(ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port));
        } else if (local.ss_family == AF_INET6) {
            bound_port_.store(ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port));
        }
    }
    running_.store(true);
    accept_thread_ = std::thread(&HttpServer::accept_loop, this);
}

void HttpServer::close_client_sockets() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& client : clients_) {
        (void)::shutdown(client->fd, SHUT_RDWR);
    }
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    frames_changed_.notify_all();
    if (listener_ >= 0) {
        (void)::shutdown(listener_, SHUT_RDWR);
        ::close(listener_);
        listener_ = -1;
    }
    close_client_sockets();
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients.swap(clients_);
    }
    for (const auto& client : clients) {
        if (client->thread.joinable()) {
            client->thread.join();
        }
        ::close(client->fd);
    }
    bound_port_.store(0);
}

void HttpServer::publish(std::string camera_id, std::vector<std::uint8_t> jpeg,
                         std::chrono::system_clock::time_point captured_at) {
    if (camera_id.empty() || jpeg.empty()) {
        return;
    }
    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(jpeg));
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        frames_[std::move(camera_id)] = {std::move(bytes), captured_at, next_version_++};
    }
    frames_changed_.notify_all();
}

void HttpServer::publish_video(std::string camera_id, const VideoPacket& packet,
                               const VideoEncodeOptions& options) {
    if (camera_id.empty() || !packet.valid())
        return;
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        video_options_[camera_id] = options;
        auto& packets = video_packets_[std::move(camera_id)];
        packets.push_back({packet, next_version_++});
        const auto newest = packet.received_at();
        while (!packets.empty() &&
               (packets.size() > 4096 ||
                newest - packets.front().packet.received_at() > std::chrono::seconds(10))) {
            packets.pop_front();
        }
    }
    frames_changed_.notify_all();
}

PublishedJpeg HttpServer::latest(const std::string& camera_id) const {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    const auto iterator = frames_.find(camera_id);
    return iterator == frames_.end() ? PublishedJpeg{} : iterator->second;
}

bool HttpServer::has_stream_clients(const std::string& camera_id) const {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    const auto iterator = stream_clients_.find(camera_id);
    return iterator != stream_clients_.end() && iterator->second > 0;
}

bool HttpServer::has_video_stream_clients(const std::string& camera_id) const {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    const auto iterator = video_stream_clients_.find(camera_id);
    return iterator != video_stream_clients_.end() && iterator->second > 0;
}

bool HttpServer::wants_jpeg(const std::string& camera_id) const {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    const auto streams = stream_clients_.find(camera_id);
    const auto requests = frame_requests_.find(camera_id);
    return (streams != stream_clients_.end() && streams->second > 0) ||
           (requests != frame_requests_.end() && requests->second > 0);
}

bool HttpServer::send_all(int fd, const void* data, std::size_t size) const {
    const auto* current = static_cast<const std::uint8_t*>(data);
    const auto deadline = std::chrono::steady_clock::now() + options_.write_timeout;
    while (size > 0 && running_.load()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return false;
        }
        pollfd descriptor{fd, POLLOUT, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        const ssize_t sent = ::send(fd, current, size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        current += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return size == 0;
}

bool HttpServer::send_text(int fd, int status, const std::string& reason,
                           const std::string& content_type, const std::string& body,
                           bool close) const {
    std::ostringstream headers;
    headers << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
            << "Server: vibe-motion\r\n"
            << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Cache-Control: no-store\r\n"
            << "Connection: " << (close ? "close" : "keep-alive") << "\r\n\r\n";
    const auto header_text = headers.str();
    return send_all(fd, header_text.data(), header_text.size()) &&
           send_all(fd, body.data(), body.size());
}

std::string HttpServer::root_page() const {
    std::vector<std::string> cameras;
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        cameras.reserve(frames_.size());
        for (const auto& entry : frames_) {
            cameras.push_back(entry.first);
        }
        for (const auto& entry : video_packets_) {
            if (std::find(cameras.begin(), cameras.end(), entry.first) == cameras.end())
                cameras.push_back(entry.first);
        }
    }
    std::sort(cameras.begin(), cameras.end());
    std::ostringstream page;
    page << "<!doctype html><html><head><meta charset=\"utf-8\"><title>vibe-motion</title></head>"
         << "<body><h1>vibe-motion</h1><p><a href=\"/status\">status</a></p>";
    for (const auto& camera : cameras) {
        page << "<section><h2>Camera " << html_escape(camera) << "</h2><img src=\"/"
             << url_escape(camera) << "/mjpg/stream\" alt=\"Camera " << html_escape(camera)
             << "\"><p><a href=\"/" << url_escape(camera)
             << "/video.mp4\">fragmented MP4 stream</a></p></section>";
    }
    page << "</body></html>";
    return page.str();
}

void HttpServer::accept_loop() {
    while (running_.load()) {
        const int fd = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!running_.load()) {
                break;
            }
            continue;
        }
        std::shared_ptr<Client> client;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto iterator = clients_.begin(); iterator != clients_.end();) {
                if ((*iterator)->done.load()) {
                    if ((*iterator)->thread.joinable()) {
                        (*iterator)->thread.join();
                    }
                    ::close((*iterator)->fd);
                    iterator = clients_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            if (clients_.size() >= options_.max_clients) {
                const std::string response = "HTTP/1.1 503 Service Unavailable\r\nConnection: "
                                             "close\r\nContent-Length: 0\r\n\r\n";
                (void)::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
                ::close(fd);
                continue;
            }
            client = std::make_shared<Client>(fd);
            clients_.push_back(client);
            client->thread = std::thread(&HttpServer::handle_client, this, client);
        }
    }
}

void HttpServer::handle_client(const std::shared_ptr<Client>& client) {
    struct ClientDone {
        const std::shared_ptr<Client>& client;
        ~ClientDone() {
            (void)::shutdown(client->fd, SHUT_RDWR);
            client->done.store(true);
        }
    } done{client};
    std::string request;
    request.reserve(2048);
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384 &&
           running_.load()) {
        pollfd descriptor{client->fd, POLLIN, 0};
        if (::poll(&descriptor, 1, 5000) <= 0) {
            client->done.store(true);
            return;
        }
        const ssize_t received = ::recv(client->fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            client->done.store(true);
            return;
        }
        request.append(buffer, static_cast<std::size_t>(received));
    }
    const auto line_end = request.find("\r\n");
    const std::string first_line = request.substr(0, line_end);
    const auto first_space = first_line.find(' ');
    const auto second_space = first_space == std::string::npos
                                  ? std::string::npos
                                  : first_line.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos ||
        first_line.substr(0, first_space) != "GET") {
        (void)send_text(client->fd, 400, "Bad Request", "text/plain", "Bad request\n");
        client->done.store(true);
        return;
    }
    std::string path = first_line.substr(first_space + 1, second_space - first_space - 1);
    if (const auto query = path.find('?'); query != std::string::npos) {
        path.resize(query);
    }
    if (path == "/") {
        (void)send_text(client->fd, 200, "OK", "text/html; charset=utf-8", root_page());
        client->done.store(true);
        return;
    }
    if (path == "/status" || path == "/status.json") {
        std::string status = status_provider_ ? status_provider_() : "{\"status\":\"running\"}";
        if (status.empty()) {
            status = "{}";
        }
        (void)send_text(client->fd, 200, "OK", "application/json", status + "\n");
        client->done.store(true);
        return;
    }

    const auto camera_end = path.find('/', 1);
    if (path.empty() || path.front() != '/' || camera_end == std::string::npos || camera_end == 1) {
        (void)send_text(client->fd, 404, "Not Found", "text/plain", "Not found\n");
        client->done.store(true);
        return;
    }
    const std::string camera = path.substr(1, camera_end - 1);
    const std::string action = path.substr(camera_end);
    if (action == "/" || action == "/stream") {
        (void)send_text(client->fd, 200, "OK", "text/html; charset=utf-8", root_page());
        client->done.store(true);
        return;
    }
    if (action == "/status" || action == "/detection/status") {
        std::string status = status_provider_ ? status_provider_() : "{\"status\":\"running\"}";
        if (status.empty())
            status = "{}";
        (void)send_text(client->fd, 200, "OK", "application/json", status + "\n");
        client->done.store(true);
        return;
    }
    if (action == "/static/stream/timestamp") {
        PublishedJpeg frame;
        {
            std::unique_lock<std::mutex> lock(frames_mutex_);
            const auto previous = frames_.find(camera);
            const auto previous_version = previous == frames_.end() ? 0U : previous->second.version;
            ++frame_requests_[camera];
            (void)frames_changed_.wait_for(lock, options_.write_timeout, [&] {
                const auto current = frames_.find(camera);
                return !running_.load() ||
                       (current != frames_.end() && current->second.version != previous_version);
            });
            const auto pending = frame_requests_.find(camera);
            if (pending != frame_requests_.end() && --pending->second == 0) {
                frame_requests_.erase(pending);
            }
            const auto current = frames_.find(camera);
            if (current != frames_.end()) {
                frame = current->second;
            }
        }
        if (!frame.bytes) {
            (void)send_text(client->fd, 404, "Not Found", "text/plain", "No frame\n");
        } else {
            std::ostringstream headers;
            headers << "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: "
                    << frame.bytes->size() << "\r\nCache-Control: no-store\r\nLast-Modified: "
                    << http_date(frame.captured_at) << "\r\nConnection: close\r\n\r\n";
            const auto text = headers.str();
            (void)(send_all(client->fd, text.data(), text.size()) &&
                   send_all(client->fd, frame.bytes->data(), frame.bytes->size()));
        }
        client->done.store(true);
        return;
    }
    if (action == "/video.mp4" || action == "/hevc.mp4") {
        std::vector<PublishedVideoPacket> initial;
        VideoEncodeOptions encode_options;
        {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            const auto found = video_packets_.find(camera);
            if (found != video_packets_.end()) {
                if (const auto options = video_options_.find(camera);
                    options != video_options_.end())
                    encode_options = options->second;
                auto begin = found->second.end();
                for (auto iterator = found->second.end(); iterator != found->second.begin();) {
                    --iterator;
                    if (iterator->packet.keyframe()) {
                        begin = iterator;
                        break;
                    }
                }
                if (begin != found->second.end())
                    initial.assign(begin, found->second.end());
            }
        }
        if (initial.empty()) {
            (void)send_text(client->fd, 503, "Service Unavailable", "text/plain",
                            "No decodable video keyframe is available\n");
            return;
        }
        FragmentedMp4Writer writer;
        std::string error;
        struct StreamOutputState {
            std::vector<std::uint8_t> initialization;
            bool initialized = false;
        };
        const auto output_state = std::make_shared<StreamOutputState>();
        if (!writer.open(
                initial.front().packet.stream(), encode_options,
                [this, fd = client->fd, output_state](const std::uint8_t* bytes, std::size_t size) {
                    if (!output_state->initialized) {
                        output_state->initialization.insert(output_state->initialization.end(),
                                                            bytes, bytes + size);
                        return true;
                    }
                    return send_all(fd, bytes, size);
                },
                &error)) {
            (void)send_text(client->fd, 503, "Service Unavailable", "text/plain", error + "\n");
            return;
        }
        const std::string headers =
            "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nCache-Control: no-store\r\n"
            "Connection: close\r\n\r\n";
        if (!send_all(client->fd, headers.data(), headers.size()) ||
            !send_all(client->fd, output_state->initialization.data(),
                      output_state->initialization.size()))
            return;
        output_state->initialized = true;
        {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            ++video_stream_clients_[camera];
        }
        std::uint64_t delivered = 0;
        for (const auto& packet : initial) {
            if (!writer.write(packet.packet, &error))
                break;
            delivered = packet.version;
        }
        while (running_.load() && error.empty()) {
            PublishedVideoPacket next;
            {
                std::unique_lock<std::mutex> lock(frames_mutex_);
                frames_changed_.wait(lock, [&] {
                    const auto found = video_packets_.find(camera);
                    return !running_.load() ||
                           (found != video_packets_.end() && !found->second.empty() &&
                            found->second.back().version > delivered);
                });
                if (!running_.load())
                    break;
                const auto& packets = video_packets_.at(camera);
                const auto found = std::find_if(packets.begin(), packets.end(), [&](const auto& p) {
                    return p.version > delivered;
                });
                if (found == packets.end())
                    continue;
                next = *found;
            }
            if (!writer.write(next.packet, &error))
                break;
            delivered = next.version;
        }
        writer.close(nullptr);
        {
            std::lock_guard<std::mutex> lock(frames_mutex_);
            const auto found = video_stream_clients_.find(camera);
            if (found != video_stream_clients_.end() && --found->second == 0)
                video_stream_clients_.erase(found);
        }
        return;
    }
    if (action != "/mjpg" && action != "/mjpg/stream") {
        (void)send_text(client->fd, 404, "Not Found", "text/plain", "Not found\n");
        client->done.store(true);
        return;
    }

    const std::string stream_headers =
        std::string("HTTP/1.1 200 OK\r\n") +
        "Content-Type: multipart/x-mixed-replace; boundary=" + boundary +
        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (!send_all(client->fd, stream_headers.data(), stream_headers.size())) {
        client->done.store(true);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        ++stream_clients_[camera];
    }
    std::uint64_t delivered = 0;
    while (running_.load()) {
        PublishedJpeg frame;
        {
            std::unique_lock<std::mutex> lock(frames_mutex_);
            frames_changed_.wait(lock, [&] {
                const auto iterator = frames_.find(camera);
                return !running_.load() ||
                       (iterator != frames_.end() && iterator->second.version != delivered);
            });
            if (!running_.load()) {
                break;
            }
            frame = frames_.at(camera);
        }
        std::ostringstream part;
        part << "--" << boundary
             << "\r\nContent-Type: image/jpeg\r\nContent-Length: " << frame.bytes->size()
             << "\r\nX-Timestamp: " << http_date(frame.captured_at) << "\r\n\r\n";
        const auto part_header = part.str();
        static constexpr char trailer[] = "\r\n";
        if (!send_all(client->fd, part_header.data(), part_header.size()) ||
            !send_all(client->fd, frame.bytes->data(), frame.bytes->size()) ||
            !send_all(client->fd, trailer, sizeof(trailer) - 1)) {
            break;
        }
        delivered = frame.version;
    }
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        const auto iterator = stream_clients_.find(camera);
        if (iterator != stream_clients_.end() && --iterator->second == 0) {
            stream_clients_.erase(iterator);
        }
    }
    client->done.store(true);
}

} // namespace vibe_motion
