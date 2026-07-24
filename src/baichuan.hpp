#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace vibe_motion {

enum class BaichuanCodec { h264, hevc };
enum class BaichuanStream { main, sub, external };
enum class BaichuanReadStatus { frame, timeout, error };
enum class BaichuanProbeStatus { available, unavailable, authentication_failed };

struct BaichuanConfig {
    std::string host;
    std::uint16_t port = 9000;
    std::string username;
    std::string password;
    std::uint8_t channel = 0;
    BaichuanStream stream = BaichuanStream::main;
    std::chrono::milliseconds open_timeout{20000};
};

struct BaichuanMetadata {
    BaichuanCodec codec = BaichuanCodec::h264;
    int width = 0;
    int height = 0;
    int fps = 0;
};

struct BaichuanFrame {
    std::vector<std::uint8_t> data;
    std::uint32_t timestamp_us = 0;
    bool keyframe = false;
};

struct BaichuanReadResult {
    BaichuanReadStatus status = BaichuanReadStatus::error;
    BaichuanFrame frame;
    std::string error;
};

struct BaichuanProbeResult {
    BaichuanProbeStatus status = BaichuanProbeStatus::unavailable;
    std::string error;
};

class BaichuanClient {
  public:
    explicit BaichuanClient(BaichuanConfig config);
    ~BaichuanClient();
    BaichuanClient(const BaichuanClient&) = delete;
    BaichuanClient& operator=(const BaichuanClient&) = delete;
    BaichuanClient(BaichuanClient&&) noexcept;
    BaichuanClient& operator=(BaichuanClient&&) noexcept;

    static BaichuanProbeResult probe(BaichuanConfig config);
    bool open(std::string* error = nullptr);
    void close() noexcept;
    bool is_open() const noexcept;
    const BaichuanMetadata& metadata() const noexcept;
    BaichuanReadResult read(std::chrono::milliseconds timeout);

  private:
    struct Impl;
    Impl* impl_;
};

} // namespace vibe_motion
