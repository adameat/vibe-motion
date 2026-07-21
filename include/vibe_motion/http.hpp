#pragma once

#include "vibe_motion/media.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vibe_motion {

struct HttpServerOptions {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = 8081;
    std::size_t max_clients = 64;
    std::chrono::milliseconds write_timeout{2000};
};

struct PublishedJpeg {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::chrono::system_clock::time_point captured_at{};
    std::uint64_t version = 0;
};

class HttpServer {
  public:
    using StatusProvider = std::function<std::string()>;

    explicit HttpServer(HttpServerOptions options = {}, StatusProvider status_provider = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void start();
    void stop();
    bool running() const noexcept {
        return running_.load();
    }
    std::uint16_t port() const noexcept {
        return bound_port_.load();
    }

    // Only the latest encoded frame per camera is retained.
    void
    publish(std::string camera_id, std::vector<std::uint8_t> jpeg,
            std::chrono::system_clock::time_point captured_at = std::chrono::system_clock::now());
    // Retains a short reference-counted packet window so a fragmented-MP4
    // client can attach at the latest keyframe without reconnecting the camera.
    void publish_video(std::string camera_id, const VideoPacket& packet,
                       const VideoEncodeOptions& options = {});
    // Retains only the latest already-encoded timelapse packet. New clients
    // deliberately wait for a keyframe published after they connect.
    void publish_timelapse_video(std::string camera_id, const VideoPacket& packet);
    PublishedJpeg latest(const std::string& camera_id) const;
    bool has_stream_clients(const std::string& camera_id) const;
    bool has_video_stream_clients(const std::string& camera_id) const;
    bool has_timelapse_stream_clients(const std::string& camera_id) const;
    bool wants_jpeg(const std::string& camera_id) const;

  private:
    struct Client;

    void accept_loop();
    void handle_client(const std::shared_ptr<Client>& client);
    bool send_all(int fd, const void* data, std::size_t size) const;
    bool send_text(int fd, int status, const std::string& reason, const std::string& content_type,
                   const std::string& body, bool close = true) const;
    std::string root_page() const;
    void close_client_sockets();

    HttpServerOptions options_;
    StatusProvider status_provider_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> bound_port_{0};
    int listener_ = -1;
    std::thread accept_thread_;

    mutable std::mutex frames_mutex_;
    std::condition_variable frames_changed_;
    std::unordered_map<std::string, PublishedJpeg> frames_;
    std::unordered_map<std::string, std::size_t> stream_clients_;
    std::unordered_map<std::string, std::size_t> frame_requests_;
    struct PublishedVideoPacket {
        VideoPacket packet;
        std::uint64_t version = 0;
    };
    std::unordered_map<std::string, std::deque<PublishedVideoPacket>> video_packets_;
    std::unordered_map<std::string, VideoEncodeOptions> video_options_;
    std::unordered_map<std::string, std::size_t> video_stream_clients_;
    std::unordered_map<std::string, PublishedVideoPacket> timelapse_packets_;
    std::unordered_map<std::string, std::size_t> timelapse_stream_clients_;
    std::uint64_t next_version_ = 1;

    mutable std::mutex clients_mutex_;
    std::vector<std::shared_ptr<Client>> clients_;
};

} // namespace vibe_motion
