#pragma once

#include "vibe_motion/frame.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vibe_motion {

// FFmpeg objects are deliberately hidden from this interface.  This keeps the
// rest of the daemon independent of FFmpeg headers and makes ownership clear.
class StreamInfo {
  public:
    StreamInfo();
    StreamInfo(const StreamInfo&);
    StreamInfo& operator=(const StreamInfo&);
    StreamInfo(StreamInfo&&) noexcept;
    StreamInfo& operator=(StreamInfo&&) noexcept;
    ~StreamInfo();

    bool valid() const noexcept;
    int width() const noexcept;
    int height() const noexcept;
    std::string codec_name() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit StreamInfo(std::shared_ptr<Impl> impl);
    friend class NetworkCameraSource;
    friend class EventMovieWriter;
    friend class VideoPacket;
};

class VideoPacket {
  public:
    VideoPacket();
    VideoPacket(const VideoPacket&);            // av_packet_ref, not a byte copy
    VideoPacket& operator=(const VideoPacket&); // av_packet_ref, not a byte copy
    VideoPacket(VideoPacket&&) noexcept;
    VideoPacket& operator=(VideoPacket&&) noexcept;
    ~VideoPacket();

    bool valid() const noexcept;
    bool keyframe() const noexcept;
    std::int64_t pts() const noexcept;
    std::int64_t dts() const noexcept;
    std::int64_t duration() const noexcept;
    std::size_t size() const noexcept;
    std::chrono::steady_clock::time_point received_at() const noexcept;
    const StreamInfo& stream() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit VideoPacket(std::unique_ptr<Impl> impl);
    friend class NetworkCameraSource;
    friend class EventMovieWriter;
};

// A reference-counted decoded color frame retained only for the duration of a
// camera sample. It allows overlays to be rendered after motion analysis.
class DecodedImage {
  public:
    DecodedImage();
    DecodedImage(const DecodedImage&);
    DecodedImage& operator=(const DecodedImage&);
    DecodedImage(DecodedImage&&) noexcept;
    DecodedImage& operator=(DecodedImage&&) noexcept;
    ~DecodedImage();

    bool valid() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit DecodedImage(std::unique_ptr<Impl> impl);
    friend class NetworkCameraSource;
    friend class TimelapseWriter;
};

struct RedBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int thickness = 2;
};

struct CameraSourceConfig {
    std::string url;
    int width = 0;              // 0 keeps the source width for motion analysis
    int height = 0;             // 0 keeps the source height for motion analysis
    int analysis_framerate = 0; // <= 0 analyzes every decoded frame
    std::chrono::milliseconds io_timeout{10000};
    std::chrono::milliseconds reconnect_delay{1000};
    std::string rtsp_transport = "tcp";
    int jpeg_quality = 80; // 1..100; <= 0 disables JPEG generation
    std::map<std::string, std::string> options;
};

struct CameraSample {
    // Invalid only for an extra frame drained from a multi-frame packet or at EOF.
    VideoPacket packet;
    // Owned by NetworkCameraSource and valid until its next read() or close().
    // Keeping the storage in the source avoids allocating a multi-megabyte buffer per frame.
    const GrayFrame* frame = nullptr;  // null when this packet produced no frame
    std::optional<DecodedImage> image; // color source for post-detection overlays
};

enum class CameraReadStatus { sample, again, timeout, end_of_stream, error };

struct CameraReadResult {
    CameraReadStatus status = CameraReadStatus::error;
    std::optional<CameraSample> sample;
    std::string error;
};

class NetworkCameraSource {
  public:
    explicit NetworkCameraSource(CameraSourceConfig config);
    ~NetworkCameraSource();
    NetworkCameraSource(const NetworkCameraSource&) = delete;
    NetworkCameraSource& operator=(const NetworkCameraSource&) = delete;
    NetworkCameraSource(NetworkCameraSource&&) noexcept;
    NetworkCameraSource& operator=(NetworkCameraSource&&) noexcept;

    bool open(std::string* error = nullptr);
    void close() noexcept;
    bool is_open() const noexcept;
    CameraReadResult read();
    const StreamInfo& stream_info() const noexcept;
    std::vector<std::uint8_t> render_jpeg(const DecodedImage& image,
                                          std::optional<RedBox> redbox = std::nullopt);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PacketRing {
  public:
    explicit PacketRing(std::chrono::milliseconds maximum_age = std::chrono::seconds{5},
                        std::size_t maximum_packets = 2048);
    void push(const VideoPacket& packet);
    void clear() noexcept;
    std::size_t size() const noexcept;
    std::vector<VideoPacket> snapshot_from_latest_keyframe() const;

  private:
    std::chrono::milliseconds maximum_age_;
    std::size_t maximum_packets_;
    std::vector<VideoPacket> packets_;
};

class EventMovieWriter {
  public:
    EventMovieWriter();
    ~EventMovieWriter();
    EventMovieWriter(const EventMovieWriter&) = delete;
    EventMovieWriter& operator=(const EventMovieWriter&) = delete;
    EventMovieWriter(EventMovieWriter&&) noexcept;
    EventMovieWriter& operator=(EventMovieWriter&&) noexcept;

    bool open(const std::string& path, const StreamInfo& stream, std::string* error = nullptr);
    bool write_preroll(const PacketRing& ring, std::string* error = nullptr);
    bool write(const VideoPacket& packet, std::string* error = nullptr);
    bool close(std::string* error = nullptr) noexcept;
    bool is_open() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TimelapseWriter {
  public:
    TimelapseWriter();
    ~TimelapseWriter();
    TimelapseWriter(const TimelapseWriter&) = delete;
    TimelapseWriter& operator=(const TimelapseWriter&) = delete;
    TimelapseWriter(TimelapseWriter&&) noexcept;
    TimelapseWriter& operator=(TimelapseWriter&&) noexcept;

    // The caller chooses the path and therefore controls hourly rotation.
    bool open(const std::string& path, int width, int height, int fps,
              std::string* error = nullptr);
    bool write(const DecodedImage& image, std::string* error = nullptr);
    bool close(std::string* error = nullptr) noexcept;
    bool is_open() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibe_motion
