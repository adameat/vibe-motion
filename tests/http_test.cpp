#include "vibe_motion/http.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vibe_motion;

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
    assert(::send(fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
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

int main() {
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
    server.stop();

    std::cout << "http tests passed\n";
}
