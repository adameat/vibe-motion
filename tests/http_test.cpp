#include "vibe_motion/http.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vibe_motion;

static void send_all(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        assert(count > 0);
        offset += static_cast<std::size_t>(count);
    }
}

static std::string get(std::uint16_t port, const std::string& path) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_all(fd, request);
    std::string response;
    char buffer[1024];
    for (;;) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    return response;
}

static std::string get_headers(std::uint16_t port, const std::string& path) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_all(fd, request);
    std::string response;
    char buffer[1024];
    while (response.find("\r\n\r\n") == std::string::npos) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        assert(count > 0);
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    return response;
}

static std::string get_until(std::uint16_t port, const std::string& path,
                             const std::string& marker) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send_all(fd, request);
    std::string response;
    char buffer[4096];
    while (response.find(marker) == std::string::npos) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && errno == EINTR)
            continue;
        assert(count > 0);
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    return response;
}

int main(int, char** argv) {
    HttpServer server({"127.0.0.1", 0, 8, std::chrono::milliseconds(1000)},
                      [] { return "{\"ok\":true}"; });
    server.start();
    assert(server.port() != 0);
    assert(!server.has_stream_clients("7"));
    assert(!server.wants_jpeg("7"));

    const auto status = get(server.port(), "/7/status");
    assert(status.find("200 OK") != std::string::npos);
    assert(status.find("{\"ok\":true}") != std::string::npos);
    std::string image;
    std::thread requester([&] { image = get(server.port(), "/7/static/stream/timestamp"); });
    for (int attempt = 0; attempt < 100 && !server.wants_jpeg("7"); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(server.wants_jpeg("7"));
    const std::vector<std::uint8_t> jpeg{0xff, 0xd8, 0x01, 0x02, 0xff, 0xd9};
    server.publish("7", jpeg);
    requester.join();
    assert(!server.wants_jpeg("7"));
    assert(image.find("Content-Type: image/jpeg") != std::string::npos);
    const auto body = image.substr(image.find("\r\n\r\n") + 4);
    assert(std::vector<std::uint8_t>(body.begin(), body.end()) == jpeg);
    const auto short_stream = get_headers(server.port(), "/7/mjpg");
    assert(short_stream.find("200 OK") != std::string::npos);
    assert(short_stream.find("multipart/x-mixed-replace") != std::string::npos);
    const auto long_stream = get_headers(server.port(), "/7/mjpg/stream");
    assert(long_stream.find("200 OK") != std::string::npos);
    assert(long_stream.find("multipart/x-mixed-replace") != std::string::npos);

    const auto fixture = std::filesystem::path(argv[0]).parent_path() / "media-fixture.mp4";
    if (std::filesystem::exists(fixture)) {
        CameraSourceConfig config;
        config.url = fixture.string();
        config.jpeg_quality = 0;
        NetworkCameraSource source(config);
        std::string error;
        std::vector<VideoPacket> packets;
        assert(source.open(&error));
        for (;;) {
            auto read = source.read();
            if (read.status == CameraReadStatus::end_of_stream)
                break;
            if (read.status == CameraReadStatus::again)
                continue;
            assert(read.status == CameraReadStatus::sample && read.sample);
            server.publish_video("7", read.sample->packet);
            if (read.sample->packet.valid())
                packets.push_back(read.sample->packet);
        }
        source.close();
        const auto video = get_headers(server.port(), "/7/video.mp4");
        assert(video.find("200 OK") != std::string::npos);
        assert(video.find("Content-Type: video/mp4") != std::string::npos);

        assert(!packets.empty() && packets.front().keyframe());
        const auto next_keyframe =
            std::find_if(std::next(packets.begin()), packets.end(),
                         [](const VideoPacket& packet) { return packet.keyframe(); });
        assert(next_keyframe != packets.end());
        server.publish_timelapse_video("7", packets.front());
        std::string timelapse_video;
        std::atomic<bool> timelapse_done{false};
        std::thread timelapse_requester([&] {
            timelapse_video = get_until(server.port(), "/7/timelapse.mp4", "moof");
            timelapse_done.store(true);
        });
        for (int attempt = 0; attempt < 100 && !server.has_timelapse_stream_clients("7");
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(server.has_timelapse_stream_clients("7"));
        for (auto packet = std::next(packets.begin()); packet != next_keyframe; ++packet)
            server.publish_timelapse_video("7", *packet);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!timelapse_done.load());
        server.publish_timelapse_video("7", *next_keyframe);
        const auto after_keyframe = std::next(next_keyframe);
        assert(after_keyframe != packets.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        server.publish_timelapse_video("7", *after_keyframe);
        timelapse_requester.join();
        assert(timelapse_video.find("200 OK") != std::string::npos);
        assert(timelapse_video.find("Content-Type: video/mp4") != std::string::npos);
        assert(timelapse_video.find("moof") != std::string::npos);
    }
    server.stop();

    std::cout << "http tests passed\n";
}
