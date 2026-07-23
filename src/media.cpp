#include "vibe_motion/media.hpp"

#include "media_internal.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace vibe_motion {

std::string normalize_video_codec(std::string codec) {
    std::transform(codec.begin(), codec.end(), codec.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (codec == "h265" || codec == "x265" || codec == "libx265") {
        return "hevc";
    }
    if (codec == "x264" || codec == "libx264") {
        return "h264";
    }
    if (codec == "passthrough") {
        return "copy";
    }
    return codec;
}

namespace {

void configure_ffmpeg_logging() noexcept {
    static const bool configured = [] {
        av_log_set_level(AV_LOG_ERROR);
        av_log_set_flags(av_log_get_flags() | AV_LOG_SKIP_REPEATED);
        return true;
    }();
    (void)configured;
}

std::string ff_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void set_error(std::string* destination, const std::string& message) {
    if (destination != nullptr) {
        *destination = message;
    }
}

std::int64_t steady_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct FormatDeleter {
    void operator()(AVFormatContext* context) const noexcept {
        if (context != nullptr) {
            avformat_free_context(context);
        }
    }
};

struct CodecDeleter {
    void operator()(AVCodecContext* context) const noexcept {
        avcodec_free_context(&context);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept {
        av_frame_free(&frame);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept {
        av_packet_free(&packet);
    }
};

using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

FramePtr make_frame() {
    return FramePtr(av_frame_alloc());
}
PacketPtr make_packet() {
    return PacketPtr(av_packet_alloc());
}

AVCodecID codec_id_for_name(const std::string& requested) {
    const std::string codec = normalize_video_codec(requested);
    if (codec == "mpeg4")
        return AV_CODEC_ID_MPEG4;
    if (codec == "h264")
        return AV_CODEC_ID_H264;
    if (codec == "hevc")
        return AV_CODEC_ID_HEVC;
    return AV_CODEC_ID_NONE;
}

bool codec_matches(AVCodecID actual, const std::string& requested) {
    const std::string codec = normalize_video_codec(requested);
    return codec == "copy" || actual == codec_id_for_name(codec);
}

const AVCodec* find_requested_encoder(const std::string& codec_name,
                                      const std::string& encoder_name) {
    const AVCodecID codec_id = codec_id_for_name(codec_name);
    if (codec_id == AV_CODEC_ID_NONE) {
        return nullptr;
    }
    const AVCodec* codec = nullptr;
    if (!encoder_name.empty()) {
        codec = avcodec_find_encoder_by_name(encoder_name.c_str());
    } else if (codec_id == AV_CODEC_ID_HEVC) {
        codec = avcodec_find_encoder_by_name("libx265");
    } else if (codec_id == AV_CODEC_ID_H264) {
        codec = avcodec_find_encoder_by_name("libx264");
    }
    if (codec == nullptr && encoder_name.empty()) {
        codec = avcodec_find_encoder(codec_id);
    }
    return codec != nullptr && codec->id == codec_id ? codec : nullptr;
}

unsigned int mp4_codec_tag(AVCodecID codec_id) {
    const auto tag = [](char a, char b, char c, char d) {
        return static_cast<unsigned int>(static_cast<unsigned char>(a)) |
               (static_cast<unsigned int>(static_cast<unsigned char>(b)) << 8U) |
               (static_cast<unsigned int>(static_cast<unsigned char>(c)) << 16U) |
               (static_cast<unsigned int>(static_cast<unsigned char>(d)) << 24U);
    };
    if (codec_id == AV_CODEC_ID_HEVC)
        return tag('h', 'v', 'c', '1');
    if (codec_id == AV_CODEC_ID_H264)
        return tag('a', 'v', 'c', '1');
    return 0;
}

bool allocate_video_frame(AVFrame* frame, AVPixelFormat format, int width, int height,
                          std::string* error) {
    frame->format = format;
    frame->width = width;
    frame->height = height;
    const int result = av_frame_get_buffer(frame, 32);
    if (result < 0) {
        set_error(error, "cannot allocate video frame: " + ff_error(result));
        return false;
    }
    return true;
}

} // namespace

bool video_encoder_available(const std::string& codec, const std::string& encoder,
                             std::string* selected_encoder) {
    const AVCodec* selected = find_requested_encoder(codec, encoder);
    if (selected_encoder != nullptr) {
        *selected_encoder = selected != nullptr ? selected->name : std::string{};
    }
    if (selected == nullptr)
        return false;
    static std::mutex cache_mutex;
    static std::map<std::string, bool> cache;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (const auto found = cache.find(selected->name); found != cache.end())
            return found->second;
    }
    auto context = CodecPtr(avcodec_alloc_context3(selected));
    if (!context)
        return false;
    context->width = 64;
    context->height = 64;
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->time_base = AVRational{1, 1};
    context->framerate = AVRational{1, 1};
    context->gop_size = 10;
    context->bit_rate = 400000;
    AVDictionary* options = nullptr;
    if (std::string(selected->name) == "libx265")
        av_dict_set(&options, "x265-params", "log-level=error", 0);
    const int result = avcodec_open2(context.get(), selected, &options);
    av_dict_free(&options);
    const bool available = result >= 0;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[selected->name] = available;
    }
    return available;
}

namespace detail {

bool decoded_frame_usable(const AVFrame* frame) noexcept {
    if (frame == nullptr) {
        return false;
    }
    // Decoders can conceal damaged slices and still return success. Both the
    // generic frame flags and codec-specific decode errors must therefore be
    // clean before motion analysis sees the pixels.
    constexpr int rejected_flags = AV_FRAME_FLAG_CORRUPT | AV_FRAME_FLAG_DISCARD;
    return (frame->flags & rejected_flags) == 0 && frame->decode_error_flags == 0;
}

void PacketTimestampNormalizer::reset(std::int64_t ticks_per_second,
                                      bool repair_from_arrival) noexcept {
    ticks_per_second_ = std::max<std::int64_t>(ticks_per_second, 1);
    first_input_.reset();
    last_input_.reset();
    first_arrival_ = 0;
    last_arrival_ = 0;
    last_output_ = 0;
    repair_from_arrival_ = repair_from_arrival;
    initialized_ = false;
}

std::int64_t PacketTimestampNormalizer::normalize(std::optional<std::int64_t> input_timestamp,
                                                  std::int64_t arrival_timestamp) noexcept {
    if (!initialized_) {
        first_input_ = input_timestamp;
        last_input_ = input_timestamp;
        first_arrival_ = arrival_timestamp;
        last_arrival_ = arrival_timestamp;
        last_output_ = 0;
        initialized_ = true;
        return 0;
    }

    if (!repair_from_arrival_) {
        std::int64_t output = last_output_ + 1;
        if (input_timestamp) {
            if (!first_input_)
                first_input_ = input_timestamp;
            output = std::max<std::int64_t>(*input_timestamp - *first_input_, output);
        }
        last_input_ = input_timestamp;
        last_arrival_ = arrival_timestamp;
        last_output_ = output;
        return last_output_;
    }

    // Some RTSP cameras reset or jump their RTP-derived DTS while continuing to
    // deliver packets normally. Preserve plausible source deltas, but use the
    // monotonic packet-arrival clock when the source timeline diverges sharply.
    const std::int64_t arrival_delta = std::max<std::int64_t>(arrival_timestamp - last_arrival_, 1);
    std::int64_t output_delta = arrival_delta;
    if (input_timestamp && last_input_) {
        const std::int64_t input_delta = *input_timestamp - *last_input_;
        const std::int64_t minimum_plausible =
            arrival_delta >= std::max<std::int64_t>(ticks_per_second_ / 100, 1)
                ? std::max<std::int64_t>(arrival_delta / 4, 1)
                : 1;
        const std::int64_t maximum_plausible =
            std::max<std::int64_t>(ticks_per_second_ * 2, arrival_delta * 4);
        if (input_delta >= minimum_plausible && input_delta <= maximum_plausible) {
            output_delta = input_delta;
        }
    }

    // A sequence of individually plausible deltas can still make a broken RTP
    // clock run much faster or slower than wall time. Keep source timing for
    // ordinary jitter, but bound cumulative skew so a short capture cannot
    // acquire long frozen sections.
    const std::int64_t arrival_elapsed =
        std::max<std::int64_t>(arrival_timestamp - first_arrival_, 0);
    const std::int64_t skew_allowance = ticks_per_second_ * 2;
    const std::int64_t lower_bound = std::max<std::int64_t>(
        last_output_ + 1, arrival_elapsed > skew_allowance ? arrival_elapsed - skew_allowance : 0);
    const std::int64_t upper_bound =
        std::max<std::int64_t>(lower_bound, arrival_elapsed + skew_allowance);
    output_delta = std::clamp(last_output_ + output_delta, lower_bound, upper_bound) - last_output_;

    last_input_ = input_timestamp;
    last_arrival_ = arrival_timestamp;
    last_output_ += output_delta;
    return last_output_;
}

} // namespace detail

struct StreamInfo::Impl {
    AVCodecParameters* parameters = nullptr;
    AVRational time_base{0, 1};
    AVRational frame_rate{0, 1};
    bool repair_timestamps_from_arrival = true;

    Impl() : parameters(avcodec_parameters_alloc()) {}
    ~Impl() {
        avcodec_parameters_free(&parameters);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

StreamInfo::StreamInfo() = default;
StreamInfo::StreamInfo(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
StreamInfo::StreamInfo(const StreamInfo&) = default;
StreamInfo& StreamInfo::operator=(const StreamInfo&) = default;
StreamInfo::StreamInfo(StreamInfo&&) noexcept = default;
StreamInfo& StreamInfo::operator=(StreamInfo&&) noexcept = default;
StreamInfo::~StreamInfo() = default;
bool StreamInfo::valid() const noexcept {
    return impl_ && impl_->parameters;
}
int StreamInfo::width() const noexcept {
    return valid() ? impl_->parameters->width : 0;
}
int StreamInfo::height() const noexcept {
    return valid() ? impl_->parameters->height : 0;
}
std::string StreamInfo::codec_name() const {
    return valid() ? avcodec_get_name(impl_->parameters->codec_id) : std::string{};
}

struct VideoPacket::Impl {
    PacketPtr packet;
    StreamInfo stream;
    std::chrono::steady_clock::time_point received{};

    Impl() : packet(make_packet()) {}
};

VideoPacket::VideoPacket() = default;
VideoPacket::VideoPacket(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VideoPacket::VideoPacket(const VideoPacket& other) {
    if (!other.valid()) {
        return;
    }
    auto copy = std::make_unique<Impl>();
    if (!copy->packet || av_packet_ref(copy->packet.get(), other.impl_->packet.get()) < 0) {
        throw std::bad_alloc();
    }
    copy->stream = other.impl_->stream;
    copy->received = other.impl_->received;
    impl_ = std::move(copy);
}
VideoPacket& VideoPacket::operator=(const VideoPacket& other) {
    if (this != &other) {
        VideoPacket copy(other);
        *this = std::move(copy);
    }
    return *this;
}
VideoPacket::VideoPacket(VideoPacket&&) noexcept = default;
VideoPacket& VideoPacket::operator=(VideoPacket&&) noexcept = default;
VideoPacket::~VideoPacket() = default;
bool VideoPacket::valid() const noexcept {
    return impl_ && impl_->packet;
}
bool VideoPacket::keyframe() const noexcept {
    return valid() && (impl_->packet->flags & AV_PKT_FLAG_KEY) != 0;
}
std::int64_t VideoPacket::pts() const noexcept {
    return valid() ? impl_->packet->pts : INT64_MIN;
}
std::int64_t VideoPacket::dts() const noexcept {
    return valid() ? impl_->packet->dts : INT64_MIN;
}
std::int64_t VideoPacket::duration() const noexcept {
    return valid() ? impl_->packet->duration : 0;
}
std::size_t VideoPacket::size() const noexcept {
    return valid() && impl_->packet->size > 0 ? static_cast<std::size_t>(impl_->packet->size) : 0;
}
std::chrono::steady_clock::time_point VideoPacket::received_at() const noexcept {
    return valid() ? impl_->received : std::chrono::steady_clock::time_point{};
}
const StreamInfo& VideoPacket::stream() const noexcept {
    static const StreamInfo empty;
    return valid() ? impl_->stream : empty;
}

struct DecodedImage::Impl {
    FramePtr frame;
    Impl() : frame(make_frame()) {}
};

DecodedImage::DecodedImage() = default;
DecodedImage::DecodedImage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DecodedImage::DecodedImage(const DecodedImage& other) {
    if (!other.valid())
        return;
    auto copy = std::make_unique<Impl>();
    if (!copy->frame || av_frame_ref(copy->frame.get(), other.impl_->frame.get()) < 0) {
        throw std::bad_alloc();
    }
    impl_ = std::move(copy);
}
DecodedImage& DecodedImage::operator=(const DecodedImage& other) {
    if (this != &other) {
        DecodedImage copy(other);
        *this = std::move(copy);
    }
    return *this;
}
DecodedImage::DecodedImage(DecodedImage&&) noexcept = default;
DecodedImage& DecodedImage::operator=(DecodedImage&&) noexcept = default;
DecodedImage::~DecodedImage() = default;
bool DecodedImage::valid() const noexcept {
    return impl_ && impl_->frame;
}

struct NetworkCameraSource::Impl {
    explicit Impl(CameraSourceConfig value) : config(std::move(value)) {}

    CameraSourceConfig config;
    AVFormatContext* format = nullptr;
    CodecPtr decoder;
    FramePtr decoded;
    SwsContext* gray_scaler = nullptr;
    SwsContext* jpeg_scaler = nullptr;
    CodecPtr jpeg_encoder;
    FramePtr jpeg_frame;
    GrayFrame gray_frame;
    int video_stream = -1;
    int output_width = 0;
    int output_height = 0;
    AVRational input_time_base{0, 1};
    std::int64_t next_analysis_timestamp = AV_NOPTS_VALUE;
    std::chrono::steady_clock::time_point next_analysis_at{};
    std::int64_t sequence = 0;
    std::int64_t jpeg_sequence = 0;
    bool input_eof = false;
    std::atomic<std::int64_t> deadline_us{0};
    StreamInfo stream;

    ~Impl() {
        close();
    }

    static int interrupt(void* opaque) noexcept {
        const auto* self = static_cast<const Impl*>(opaque);
        const std::int64_t deadline = self->deadline_us.load(std::memory_order_relaxed);
        return deadline > 0 && steady_us() >= deadline ? 1 : 0;
    }

    void arm_timeout() noexcept {
        if (config.io_timeout.count() <= 0) {
            deadline_us.store(0, std::memory_order_relaxed);
        } else {
            const auto delta =
                std::chrono::duration_cast<std::chrono::microseconds>(config.io_timeout);
            deadline_us.store(steady_us() + delta.count(), std::memory_order_relaxed);
        }
    }

    void close() noexcept {
        deadline_us.store(0, std::memory_order_relaxed);
        sws_freeContext(gray_scaler);
        gray_scaler = nullptr;
        sws_freeContext(jpeg_scaler);
        jpeg_scaler = nullptr;
        jpeg_frame.reset();
        jpeg_encoder.reset();
        decoded.reset();
        decoder.reset();
        if (format != nullptr) {
            avformat_close_input(&format);
        }
        video_stream = -1;
        output_width = 0;
        output_height = 0;
        input_time_base = AVRational{0, 1};
        next_analysis_timestamp = AV_NOPTS_VALUE;
        next_analysis_at = {};
        jpeg_sequence = 0;
        input_eof = false;
        stream = StreamInfo{};
    }

    bool should_analyze(const AVFrame* frame) {
        if (config.analysis_framerate <= 0) {
            return true;
        }
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE && input_time_base.num > 0 &&
            input_time_base.den > 0) {
            const auto interval = std::max<std::int64_t>(
                1, av_rescale_q(1, AVRational{1, config.analysis_framerate}, input_time_base));
            const auto timestamp = frame->best_effort_timestamp;
            if (next_analysis_timestamp == AV_NOPTS_VALUE ||
                timestamp + interval * 2 < next_analysis_timestamp ||
                timestamp >= next_analysis_timestamp) {
                next_analysis_timestamp = timestamp + interval;
                return true;
            }
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto interval = std::chrono::microseconds(1000000 / config.analysis_framerate);
        if (next_analysis_at.time_since_epoch().count() == 0 || now >= next_analysis_at) {
            next_analysis_at = now + interval;
            return true;
        }
        return false;
    }

    bool init_jpeg(std::string* error) {
        if (config.jpeg_quality <= 0) {
            return true;
        }
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (codec == nullptr) {
            set_error(error, "FFmpeg MJPEG encoder is unavailable");
            return false;
        }
        jpeg_encoder.reset(avcodec_alloc_context3(codec));
        jpeg_frame = make_frame();
        if (!jpeg_encoder || !jpeg_frame) {
            set_error(error, "cannot allocate JPEG encoder");
            return false;
        }
        jpeg_encoder->width = output_width;
        jpeg_encoder->height = output_height;
        jpeg_encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;
        jpeg_encoder->time_base = AVRational{1, 25};
        jpeg_encoder->flags |= AV_CODEC_FLAG_QSCALE;
        const int quality = std::clamp(config.jpeg_quality, 1, 100);
        jpeg_encoder->global_quality = FF_QP2LAMBDA * (31 - ((quality - 1) * 29 / 99));
        int result = avcodec_open2(jpeg_encoder.get(), codec, nullptr);
        if (result < 0) {
            set_error(error, "cannot open JPEG encoder: " + ff_error(result));
            return false;
        }
        return allocate_video_frame(jpeg_frame.get(), jpeg_encoder->pix_fmt, output_width,
                                    output_height, error);
    }

    void draw_redbox(const RedBox& requested) {
        if (!jpeg_frame || jpeg_frame->format != AV_PIX_FMT_YUVJ420P)
            return;
        const int left = std::clamp(requested.left, 0, output_width - 1);
        const int right = std::clamp(requested.right, 0, output_width - 1);
        const int top = std::clamp(requested.top, 0, output_height - 1);
        const int bottom = std::clamp(requested.bottom, 0, output_height - 1);
        if (left > right || top > bottom)
            return;
        const int thickness = std::clamp(requested.thickness, 1,
                                         std::max(1, std::min(output_width, output_height) / 4));
        const auto paint = [this](int x, int y) {
            // Full-range BT.601 red in YUV420.
            jpeg_frame->data[0][y * jpeg_frame->linesize[0] + x] = 76;
            jpeg_frame->data[1][(y / 2) * jpeg_frame->linesize[1] + x / 2] = 85;
            jpeg_frame->data[2][(y / 2) * jpeg_frame->linesize[2] + x / 2] = 255;
        };
        for (int offset = 0; offset < thickness; ++offset) {
            const int y1 = std::min(bottom, top + offset);
            const int y2 = std::max(top, bottom - offset);
            for (int x = left; x <= right; ++x) {
                paint(x, y1);
                paint(x, y2);
            }
            const int x1 = std::min(right, left + offset);
            const int x2 = std::max(left, right - offset);
            for (int y = top; y <= bottom; ++y) {
                paint(x1, y);
                paint(x2, y);
            }
        }
    }

    std::vector<std::uint8_t> encode_jpeg(const AVFrame* source,
                                          std::optional<RedBox> redbox = std::nullopt) {
        std::vector<std::uint8_t> bytes;
        if (!jpeg_encoder) {
            return bytes;
        }
        jpeg_scaler = sws_getCachedContext(jpeg_scaler, source->width, source->height,
                                           static_cast<AVPixelFormat>(source->format), output_width,
                                           output_height, jpeg_encoder->pix_fmt, SWS_BILINEAR,
                                           nullptr, nullptr, nullptr);
        if (jpeg_scaler == nullptr || av_frame_make_writable(jpeg_frame.get()) < 0) {
            return bytes;
        }
        sws_scale(jpeg_scaler, source->data, source->linesize, 0, source->height, jpeg_frame->data,
                  jpeg_frame->linesize);
        if (redbox)
            draw_redbox(*redbox);
        jpeg_frame->pts = ++jpeg_sequence;
        if (avcodec_send_frame(jpeg_encoder.get(), jpeg_frame.get()) < 0) {
            return bytes;
        }
        auto encoded = make_packet();
        if (!encoded || avcodec_receive_packet(jpeg_encoder.get(), encoded.get()) < 0) {
            return bytes;
        }
        bytes.assign(encoded->data, encoded->data + encoded->size);
        return bytes;
    }

    const GrayFrame* convert_gray(const AVFrame* source) {
        gray_frame.width = output_width;
        gray_frame.height = output_height;
        gray_frame.sequence = ++sequence;
        gray_frame.captured_at = std::chrono::system_clock::now();
        gray_frame.pixels.resize(static_cast<std::size_t>(output_width) *
                                 static_cast<std::size_t>(output_height));
        const auto pixel_format = static_cast<AVPixelFormat>(source->format);
        const bool direct_luma =
            pixel_format == AV_PIX_FMT_GRAY8 || pixel_format == AV_PIX_FMT_YUV420P ||
            pixel_format == AV_PIX_FMT_YUVJ420P || pixel_format == AV_PIX_FMT_YUV422P ||
            pixel_format == AV_PIX_FMT_YUVJ422P || pixel_format == AV_PIX_FMT_YUV444P ||
            pixel_format == AV_PIX_FMT_YUVJ444P || pixel_format == AV_PIX_FMT_NV12 ||
            pixel_format == AV_PIX_FMT_NV21;
        if (direct_luma && source->width == output_width && source->height == output_height &&
            source->data[0] != nullptr && source->linesize[0] >= output_width) {
            for (int row = 0; row < output_height; ++row) {
                std::memcpy(gray_frame.pixels.data() + static_cast<std::size_t>(row) *
                                                           static_cast<std::size_t>(output_width),
                            source->data[0] +
                                static_cast<std::ptrdiff_t>(row) * source->linesize[0],
                            static_cast<std::size_t>(output_width));
            }
            return &gray_frame;
        }
        std::uint8_t* destination[] = {gray_frame.pixels.data(), nullptr, nullptr, nullptr};
        int lines[] = {output_width, 0, 0, 0};
        gray_scaler = sws_getCachedContext(
            gray_scaler, source->width, source->height, static_cast<AVPixelFormat>(source->format),
            output_width, output_height, AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (gray_scaler == nullptr || sws_scale(gray_scaler, source->data, source->linesize, 0,
                                                source->height, destination, lines) <= 0) {
            return nullptr;
        }
        return &gray_frame;
    }
};

NetworkCameraSource::NetworkCameraSource(CameraSourceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
    configure_ffmpeg_logging();
}
NetworkCameraSource::~NetworkCameraSource() = default;
NetworkCameraSource::NetworkCameraSource(NetworkCameraSource&&) noexcept = default;
NetworkCameraSource& NetworkCameraSource::operator=(NetworkCameraSource&&) noexcept = default;

bool NetworkCameraSource::open(std::string* error) {
    impl_->close();
    if (impl_->config.url.empty()) {
        set_error(error, "camera URL is empty");
        return false;
    }
    AVFormatContext* context = avformat_alloc_context();
    if (context == nullptr) {
        set_error(error, "cannot allocate FFmpeg input context");
        return false;
    }
    context->flags |= AVFMT_FLAG_DISCARD_CORRUPT;
    context->interrupt_callback = AVIOInterruptCB{&Impl::interrupt, impl_.get()};
    AVDictionary* options = nullptr;
    if (!impl_->config.rtsp_transport.empty() && impl_->config.url.rfind("rtsp", 0) == 0) {
        av_dict_set(&options, "rtsp_transport", impl_->config.rtsp_transport.c_str(), 0);
    }
    for (const auto& option : impl_->config.options) {
        av_dict_set(&options, option.first.c_str(), option.second.c_str(), 0);
    }
    impl_->arm_timeout();
    int result = avformat_open_input(&context, impl_->config.url.c_str(), nullptr, &options);
    av_dict_free(&options);
    impl_->deadline_us.store(0, std::memory_order_relaxed);
    if (result < 0) {
        if (context != nullptr) {
            avformat_free_context(context);
        }
        set_error(error, "cannot open camera " + impl_->config.url + ": " + ff_error(result));
        return false;
    }
    impl_->format = context;
    impl_->arm_timeout();
    result = avformat_find_stream_info(context, nullptr);
    impl_->deadline_us.store(0, std::memory_order_relaxed);
    if (result < 0) {
        set_error(error, "cannot read camera stream information: " + ff_error(result));
        impl_->close();
        return false;
    }
    result = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (result < 0) {
        set_error(error, "camera has no decodable video stream: " + ff_error(result));
        impl_->close();
        return false;
    }
    impl_->video_stream = result;
    AVStream* input_stream = context->streams[result];
    impl_->input_time_base = input_stream->time_base;
    const AVCodec* codec = avcodec_find_decoder(input_stream->codecpar->codec_id);
    if (codec == nullptr) {
        set_error(error, "no decoder for camera codec");
        impl_->close();
        return false;
    }
    impl_->decoder.reset(avcodec_alloc_context3(codec));
    impl_->decoded = make_frame();
    if (!impl_->decoder || !impl_->decoded) {
        set_error(error, "cannot allocate camera decoder");
        impl_->close();
        return false;
    }
    result = avcodec_parameters_to_context(impl_->decoder.get(), input_stream->codecpar);
    if (result >= 0) {
        impl_->decoder->skip_frame = impl_->config.decode_mode == FrameDecodeMode::keyframes
                                         ? AVDISCARD_NONKEY
                                         : AVDISCARD_DEFAULT;
        result = avcodec_open2(impl_->decoder.get(), codec, nullptr);
    }
    if (result < 0) {
        set_error(error, "cannot open camera decoder: " + ff_error(result));
        impl_->close();
        return false;
    }
    auto stream_impl = std::make_shared<StreamInfo::Impl>();
    if (!stream_impl->parameters ||
        avcodec_parameters_copy(stream_impl->parameters, input_stream->codecpar) < 0) {
        set_error(error, "cannot copy camera codec parameters");
        impl_->close();
        return false;
    }
    stream_impl->time_base = input_stream->time_base;
    stream_impl->frame_rate = av_guess_frame_rate(context, input_stream, nullptr);
    impl_->stream = StreamInfo(std::move(stream_impl));
    impl_->output_width = impl_->config.width > 0 ? impl_->config.width : impl_->decoder->width;
    impl_->output_height = impl_->config.height > 0 ? impl_->config.height : impl_->decoder->height;
    if (impl_->output_width <= 0 || impl_->output_height <= 0 || !impl_->init_jpeg(error)) {
        if (impl_->output_width <= 0 || impl_->output_height <= 0) {
            set_error(error, "camera reported an invalid frame size");
        }
        impl_->close();
        return false;
    }
    return true;
}

void NetworkCameraSource::close() noexcept {
    impl_->close();
}
bool NetworkCameraSource::is_open() const noexcept {
    return impl_ && impl_->format != nullptr;
}
const StreamInfo& NetworkCameraSource::stream_info() const noexcept {
    return impl_->stream;
}
FrameDecodeMode NetworkCameraSource::decode_mode() const noexcept {
    return impl_->config.decode_mode;
}
void NetworkCameraSource::set_decode_mode(FrameDecodeMode mode) noexcept {
    impl_->config.decode_mode = mode;
    if (impl_->decoder) {
        impl_->decoder->skip_frame =
            mode == FrameDecodeMode::keyframes ? AVDISCARD_NONKEY : AVDISCARD_DEFAULT;
    }
}

CameraReadResult NetworkCameraSource::read() {
    if (!is_open()) {
        return {CameraReadStatus::error, std::nullopt, "camera is not open"};
    }
    auto decoded_sample = [this](CameraSample sample = {}) -> CameraReadResult {
        if (!detail::decoded_frame_usable(impl_->decoded.get())) {
            return {CameraReadStatus::sample, std::move(sample), {}};
        }
#ifdef AV_FRAME_FLAG_KEY
        sample.decoded_keyframe = (impl_->decoded->flags & AV_FRAME_FLAG_KEY) != 0;
#else
        sample.decoded_keyframe = impl_->decoded->key_frame != 0;
#endif
        // Some RTSP cameras omit dimensions from the initial codec parameters
        // and reveal them only after the first frame is decoded. StreamInfo is
        // shared with retained packets and movie writers, so keep it current.
        if (impl_->stream.impl_ && impl_->stream.impl_->parameters) {
            if (impl_->stream.impl_->parameters->width <= 0) {
                impl_->stream.impl_->parameters->width = impl_->decoded->width;
            }
            if (impl_->stream.impl_->parameters->height <= 0) {
                impl_->stream.impl_->parameters->height = impl_->decoded->height;
            }
        }
        if (impl_->should_analyze(impl_->decoded.get())) {
            sample.frame = impl_->convert_gray(impl_->decoded.get());
        }
        if (sample.frame) {
            auto retained = std::make_unique<DecodedImage::Impl>();
            if (!retained->frame || av_frame_ref(retained->frame.get(), impl_->decoded.get()) < 0) {
                return {CameraReadStatus::error, std::nullopt,
                        "cannot retain decoded camera frame"};
            }
            sample.image = DecodedImage(std::move(retained));
        }
        return {CameraReadStatus::sample, std::move(sample), {}};
    };

    // Drain all frames produced by the previous packet before reading another
    // packet. Some codecs produce more than one frame per compressed packet.
    av_frame_unref(impl_->decoded.get());
    int result = avcodec_receive_frame(impl_->decoder.get(), impl_->decoded.get());
    if (result >= 0) {
        return decoded_sample();
    }
    if (result == AVERROR_EOF) {
        return {CameraReadStatus::end_of_stream, std::nullopt, {}};
    }
    if (result != AVERROR(EAGAIN)) {
        return {CameraReadStatus::error, std::nullopt, "video decode failed: " + ff_error(result)};
    }
    if (impl_->input_eof) {
        return {CameraReadStatus::end_of_stream, std::nullopt, {}};
    }

    auto input = make_packet();
    if (!input) {
        return {CameraReadStatus::error, std::nullopt, "cannot allocate input packet"};
    }
    for (;;) {
        impl_->arm_timeout();
        result = av_read_frame(impl_->format, input.get());
        impl_->deadline_us.store(0, std::memory_order_relaxed);
        if (result < 0) {
            const bool timed_out = result == AVERROR_EXIT || result == AVERROR(ETIMEDOUT);
            if (timed_out) {
                return {CameraReadStatus::timeout, std::nullopt, ff_error(result)};
            }
            if (result == AVERROR(EAGAIN)) {
                return {CameraReadStatus::again, std::nullopt, {}};
            }
            if (result == AVERROR_EOF) {
                impl_->input_eof = true;
                result = avcodec_send_packet(impl_->decoder.get(), nullptr);
                if (result < 0 && result != AVERROR_EOF) {
                    return {CameraReadStatus::error, std::nullopt,
                            "cannot flush video decoder: " + ff_error(result)};
                }
                av_frame_unref(impl_->decoded.get());
                result = avcodec_receive_frame(impl_->decoder.get(), impl_->decoded.get());
                if (result >= 0) {
                    return decoded_sample();
                }
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                    return {CameraReadStatus::end_of_stream, std::nullopt, {}};
                }
                return {CameraReadStatus::error, std::nullopt,
                        "video decoder flush failed: " + ff_error(result)};
            }
            return {CameraReadStatus::error, std::nullopt, ff_error(result)};
        }
        if (input->stream_index == impl_->video_stream) {
            break;
        }
        av_packet_unref(input.get());
    }

    auto packet_impl = std::make_unique<VideoPacket::Impl>();
    if (!packet_impl->packet || av_packet_ref(packet_impl->packet.get(), input.get()) < 0) {
        return {CameraReadStatus::error, std::nullopt, "cannot retain camera packet"};
    }
    packet_impl->stream = impl_->stream;
    packet_impl->received = std::chrono::steady_clock::now();
    CameraSample sample;
    sample.packet = VideoPacket(std::move(packet_impl));

    result = avcodec_send_packet(impl_->decoder.get(), input.get());
    if (result >= 0) {
        av_frame_unref(impl_->decoded.get());
        result = avcodec_receive_frame(impl_->decoder.get(), impl_->decoded.get());
        if (result >= 0) {
            return decoded_sample(std::move(sample));
        }
    }
    if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        return {CameraReadStatus::error, std::nullopt, "video decode failed: " + ff_error(result)};
    }
    return {CameraReadStatus::sample, std::move(sample), {}};
}

std::vector<std::uint8_t> NetworkCameraSource::render_jpeg(const DecodedImage& image,
                                                           std::optional<RedBox> redbox) {
    if (!is_open() || !image.valid())
        return {};
    return impl_->encode_jpeg(image.impl_->frame.get(), redbox);
}

PacketRing::PacketRing(std::chrono::milliseconds maximum_age, std::size_t maximum_packets)
    : maximum_age_(maximum_age), maximum_packets_(maximum_packets) {
    if (maximum_age_.count() < 0 || maximum_packets_ == 0) {
        throw std::invalid_argument("invalid packet ring limits");
    }
}

void PacketRing::push(const VideoPacket& packet) {
    if (!packet.valid()) {
        return;
    }
    packets_.push_back(packet);
    const auto newest = packet.received_at();
    auto first = packets_.begin();
    while (
        first != packets_.end() &&
        (packets_.size() - static_cast<std::size_t>(first - packets_.begin()) > maximum_packets_ ||
         newest - first->received_at() > maximum_age_)) {
        ++first;
    }
    packets_.erase(packets_.begin(), first);
}
void PacketRing::clear() noexcept {
    packets_.clear();
}
std::size_t PacketRing::size() const noexcept {
    return packets_.size();
}
std::vector<VideoPacket> PacketRing::snapshot_from_latest_keyframe() const {
    if (packets_.empty()) {
        return {};
    }
    auto begin = packets_.begin();
    for (auto iterator = packets_.end(); iterator != packets_.begin();) {
        --iterator;
        if (iterator->keyframe()) {
            begin = iterator;
            break;
        }
    }
    if (!begin->keyframe()) {
        return {};
    }
    return {begin, packets_.end()};
}

struct PacketTranscoder {
    AVFormatContext* format = nullptr;
    AVStream* output_stream = nullptr;
    StreamInfo source;
    CodecPtr decoder;
    CodecPtr encoder;
    FramePtr decoded;
    FramePtr converted;
    SwsContext* scaler = nullptr;
    AVRational encoder_time_base{0, 1};
    std::int64_t base_pts = AV_NOPTS_VALUE;
    std::int64_t last_pts = AV_NOPTS_VALUE;
    bool started = false;
    bool flushed = false;

    ~PacketTranscoder() {
        sws_freeContext(scaler);
    }

    bool open(AVFormatContext* output_format, const StreamInfo& input,
              const VideoEncodeOptions& options, std::string* error) {
        format = output_format;
        source = input;
        const AVCodec* decoder_codec = avcodec_find_decoder(input.impl_->parameters->codec_id);
        const AVCodec* encoder_codec = find_requested_encoder(options.codec, options.encoder);
        decoder.reset(decoder_codec ? avcodec_alloc_context3(decoder_codec) : nullptr);
        encoder.reset(encoder_codec ? avcodec_alloc_context3(encoder_codec) : nullptr);
        decoded = make_frame();
        converted = make_frame();
        if (!format || !decoder || !encoder || !decoded || !converted) {
            set_error(error, "cannot allocate video transcoder");
            return false;
        }
        int result = avcodec_parameters_to_context(decoder.get(), input.impl_->parameters);
        decoder->pkt_timebase = input.impl_->time_base;
        if (result >= 0)
            result = avcodec_open2(decoder.get(), decoder_codec, nullptr);
        if (result < 0) {
            set_error(error, "cannot open transcode decoder: " + ff_error(result));
            return false;
        }

        const int width = input.impl_->parameters->width;
        const int height = input.impl_->parameters->height;
        AVRational frame_rate = input.impl_->frame_rate;
        if (width <= 0 || height <= 0) {
            set_error(error, "cannot transcode a stream with unknown dimensions");
            return false;
        }
        if (frame_rate.num <= 0 || frame_rate.den <= 0)
            frame_rate = AVRational{25, 1};
        encoder_time_base = av_inv_q(frame_rate);
        encoder->codec_type = AVMEDIA_TYPE_VIDEO;
        encoder->codec_id = encoder_codec->id;
        encoder->width = width;
        encoder->height = height;
        encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder->time_base = encoder_time_base;
        encoder->framerate = frame_rate;
        const double fps = av_q2d(frame_rate);
        encoder->gop_size = std::max(1, static_cast<int>(fps * options.keyframe_interval + 0.5));
        encoder->keyint_min = encoder->gop_size;
        encoder->max_b_frames = 0;
        AVDictionary* encoder_options = nullptr;
        if (options.quality > 0) {
            const std::string crf = std::to_string((100 - options.quality) * 51 / 100);
            av_dict_set(&encoder_options, "crf", crf.c_str(), 0);
        } else if (options.bitrate > 0) {
            encoder->bit_rate = options.bitrate;
        } else {
            encoder->bit_rate = std::max<std::int64_t>(
                400000, static_cast<std::int64_t>(width) * height *
                            std::max<std::int64_t>(1, static_cast<std::int64_t>(fps)) / 5);
        }
        if (std::string(encoder_codec->name) == "libx265")
            av_dict_set(&encoder_options, "x265-params", "log-level=error", 0);
        if (options.low_latency) {
            av_dict_set(&encoder_options, "preset", "veryfast", 0);
            av_dict_set(&encoder_options, "tune", "zerolatency", 0);
        }
        if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0)
            encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        result = avcodec_open2(encoder.get(), encoder_codec, &encoder_options);
        av_dict_free(&encoder_options);
        if (result < 0) {
            set_error(error, "cannot open transcode encoder: " + ff_error(result));
            return false;
        }
        if (!allocate_video_frame(converted.get(), encoder->pix_fmt, width, height, error))
            return false;
        output_stream = avformat_new_stream(format, nullptr);
        if (!output_stream) {
            set_error(error, "cannot allocate transcoded output stream");
            return false;
        }
        result = avcodec_parameters_from_context(output_stream->codecpar, encoder.get());
        if (result < 0) {
            set_error(error, "cannot copy transcoded codec parameters: " + ff_error(result));
            return false;
        }
        output_stream->time_base = encoder_time_base;
        output_stream->avg_frame_rate = frame_rate;
        output_stream->codecpar->codec_tag = 0;
        return true;
    }

    bool write_encoded_packets(std::string* error) {
        auto packet = make_packet();
        if (!packet) {
            set_error(error, "cannot allocate encoded packet");
            return false;
        }
        for (;;) {
            const int result = avcodec_receive_packet(encoder.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return true;
            if (result < 0) {
                set_error(error, "cannot receive transcoded packet: " + ff_error(result));
                return false;
            }
            // Older MP4 muxers can mark the final zero-duration packet as
            // discard when no following DTS is available to infer its span.
            if (packet->duration <= 0)
                packet->duration = 1;
            av_packet_rescale_ts(packet.get(), encoder->time_base, output_stream->time_base);
            packet->stream_index = output_stream->index;
            packet->pos = -1;
            const int write_result = av_interleaved_write_frame(format, packet.get());
            if (write_result < 0) {
                set_error(error, "cannot write transcoded packet: " + ff_error(write_result));
                return false;
            }
            av_packet_unref(packet.get());
        }
    }

    bool encode_frame(AVFrame* input, std::string* error) {
        scaler = sws_getCachedContext(
            scaler, input->width, input->height, static_cast<AVPixelFormat>(input->format),
            converted->width, converted->height, static_cast<AVPixelFormat>(converted->format),
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler || av_frame_make_writable(converted.get()) < 0 ||
            sws_scale(scaler, input->data, input->linesize, 0, input->height, converted->data,
                      converted->linesize) <= 0) {
            set_error(error, "cannot convert frame for transcoding");
            return false;
        }
        std::int64_t pts = input->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
            pts = input->pts;
        if (base_pts == AV_NOPTS_VALUE)
            base_pts = pts;
        if (pts != AV_NOPTS_VALUE && base_pts != AV_NOPTS_VALUE)
            pts = av_rescale_q(pts - base_pts, source.impl_->time_base, encoder_time_base);
        if (pts == AV_NOPTS_VALUE || (last_pts != AV_NOPTS_VALUE && pts <= last_pts))
            pts = last_pts == AV_NOPTS_VALUE ? 0 : last_pts + 1;
        last_pts = pts;
        converted->pts = pts;
        converted->pict_type = AV_PICTURE_TYPE_NONE;
        int result = avcodec_send_frame(encoder.get(), converted.get());
        if (result == AVERROR(EAGAIN)) {
            if (!write_encoded_packets(error))
                return false;
            result = avcodec_send_frame(encoder.get(), converted.get());
        }
        if (result < 0) {
            set_error(error, "cannot submit transcoded frame: " + ff_error(result));
            return false;
        }
        return write_encoded_packets(error);
    }

    bool receive_frames(std::string* error) {
        for (;;) {
            av_frame_unref(decoded.get());
            const int result = avcodec_receive_frame(decoder.get(), decoded.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                return true;
            if (result < 0) {
                set_error(error, "cannot decode packet for transcoding: " + ff_error(result));
                return false;
            }
            if (!detail::decoded_frame_usable(decoded.get()))
                continue;
            if (!encode_frame(decoded.get(), error))
                return false;
        }
    }

    bool write(const VideoPacket& input, std::string* error) {
        if (!input.valid())
            return true;
        if (!started && !input.keyframe())
            return true;
        started = true;
        int result = avcodec_send_packet(decoder.get(), input.impl_->packet.get());
        if (result == AVERROR(EAGAIN)) {
            if (!receive_frames(error))
                return false;
            result = avcodec_send_packet(decoder.get(), input.impl_->packet.get());
        }
        if (result < 0) {
            set_error(error, "cannot submit packet for transcoding: " + ff_error(result));
            return false;
        }
        return receive_frames(error);
    }

    bool flush(std::string* error) {
        if (flushed)
            return true;

        for (;;) {
            const int result = avcodec_send_packet(decoder.get(), nullptr);
            if (result == AVERROR(EAGAIN)) {
                if (!receive_frames(error))
                    return false;
                continue;
            }
            if (result < 0 && result != AVERROR_EOF) {
                set_error(error, "cannot flush transcode decoder: " + ff_error(result));
                return false;
            }
            break;
        }
        if (!receive_frames(error))
            return false;

        for (;;) {
            const int result = avcodec_send_frame(encoder.get(), nullptr);
            if (result == AVERROR(EAGAIN)) {
                if (!write_encoded_packets(error))
                    return false;
                continue;
            }
            if (result < 0 && result != AVERROR_EOF) {
                set_error(error, "cannot flush transcode encoder: " + ff_error(result));
                return false;
            }
            break;
        }
        if (!write_encoded_packets(error))
            return false;
        flushed = true;
        return true;
    }
};

struct EventMovieWriter::Impl {
    AVFormatContext* format = nullptr;
    AVStream* output_stream = nullptr;
    StreamInfo source;
    bool header_written = false;
    bool wrote_packet = false;
    detail::PacketTimestampNormalizer timestamps;
    std::unique_ptr<PacketTranscoder> transcoder;

    ~Impl() {
        close(nullptr);
    }

    bool write(const VideoPacket& input, std::string* error) {
        if (!format) {
            set_error(error, "movie writer is not open");
            return false;
        }
        if (!input.valid()) {
            return true; // Decoder-drain samples do not carry a new packet.
        }
        if (transcoder)
            return transcoder->write(input, error);
        if (!input.stream().valid()) {
            set_error(error, "movie packet has no stream information");
            return false;
        }
        if (input.stream().impl_->parameters->codec_id != source.impl_->parameters->codec_id) {
            set_error(error, "packet codec does not match movie stream");
            return false;
        }
        if ((input.impl_->packet->flags & AV_PKT_FLAG_DISCARD) != 0) {
            return true;
        }
        if (!wrote_packet && !input.keyframe()) {
            return true;
        }
        auto packet = make_packet();
        if (!packet || av_packet_ref(packet.get(), input.impl_->packet.get()) < 0) {
            set_error(error, "cannot clone movie packet");
            return false;
        }
        const std::int64_t source_pts_delta =
            packet->pts != AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE
                ? packet->pts - packet->dts
                : 0;
        av_packet_rescale_ts(packet.get(), source.impl_->time_base, output_stream->time_base);
        const auto input_dts =
            packet->dts != AV_NOPTS_VALUE
                ? std::optional<std::int64_t>{packet->dts}
                : (packet->pts != AV_NOPTS_VALUE ? std::optional<std::int64_t>{packet->pts}
                                                 : std::nullopt);
        const auto arrival_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    input.received_at().time_since_epoch())
                                    .count();
        const auto arrival_timestamp =
            av_rescale_q(arrival_us, AVRational{1, 1000000}, output_stream->time_base);
        packet->dts = timestamps.normalize(input_dts, arrival_timestamp);
        const auto pts_delta =
            source_pts_delta > 0
                ? av_rescale_q(source_pts_delta, source.impl_->time_base, output_stream->time_base)
                : 0;
        packet->pts = packet->dts + std::max<std::int64_t>(pts_delta, 0);
        packet->stream_index = output_stream->index;
        packet->pos = -1;
        const int result = av_interleaved_write_frame(format, packet.get());
        if (result < 0) {
            set_error(error, "cannot write movie packet: " + ff_error(result));
            return false;
        }
        wrote_packet = true;
        return true;
    }

    bool close(std::string* error) noexcept {
        bool success = true;
        if (format != nullptr) {
            if (transcoder && header_written) {
                try {
                    success = transcoder->flush(error);
                } catch (...) {
                    success = false;
                }
            }
            if (header_written) {
                const int result = av_write_trailer(format);
                if (result < 0) {
                    success = false;
                    try {
                        set_error(error, "cannot finalize movie: " + ff_error(result));
                    } catch (...) {
                    }
                }
            }
            if ((format->oformat->flags & AVFMT_NOFILE) == 0 && format->pb != nullptr) {
                avio_closep(&format->pb);
            }
            avformat_free_context(format);
        }
        format = nullptr;
        output_stream = nullptr;
        source = StreamInfo{};
        header_written = false;
        wrote_packet = false;
        timestamps.reset(1);
        transcoder.reset();
        return success;
    }
};

EventMovieWriter::EventMovieWriter() : impl_(std::make_unique<Impl>()) {
    configure_ffmpeg_logging();
}
EventMovieWriter::~EventMovieWriter() = default;
EventMovieWriter::EventMovieWriter(EventMovieWriter&&) noexcept = default;
EventMovieWriter& EventMovieWriter::operator=(EventMovieWriter&&) noexcept = default;
bool EventMovieWriter::open(const std::string& path, const StreamInfo& stream, std::string* error) {
    return open(path, stream, VideoEncodeOptions{}, error);
}
bool EventMovieWriter::open(const std::string& path, const StreamInfo& stream,
                            const VideoEncodeOptions& options, std::string* error) {
    impl_->close(nullptr);
    if (!stream.valid()) {
        set_error(error, "cannot open movie without stream information");
        return false;
    }
    int result = avformat_alloc_output_context2(&impl_->format, nullptr, nullptr, path.c_str());
    if (result < 0 || impl_->format == nullptr) {
        set_error(error, "cannot select movie container: " + ff_error(result));
        return false;
    }
    const std::string codec = normalize_video_codec(options.codec);
    const AVCodecID output_codec =
        codec == "copy" ? stream.impl_->parameters->codec_id : codec_id_for_name(codec);
    if (output_codec == AV_CODEC_ID_NONE) {
        set_error(error, "unsupported movie codec " + options.codec);
        impl_->close(nullptr);
        return false;
    }
    if (avformat_query_codec(impl_->format->oformat, output_codec, FF_COMPLIANCE_NORMAL) == 0) {
        set_error(error, "movie container does not support output codec " +
                             std::string(avcodec_get_name(output_codec)));
        impl_->close(nullptr);
        return false;
    }
    if (codec == "copy") {
        impl_->output_stream = avformat_new_stream(impl_->format, nullptr);
        if (impl_->output_stream == nullptr) {
            set_error(error, "cannot allocate output video stream");
            impl_->close(nullptr);
            return false;
        }
        result = avcodec_parameters_copy(impl_->output_stream->codecpar, stream.impl_->parameters);
        if (result < 0) {
            set_error(error, "cannot copy output codec parameters: " + ff_error(result));
            impl_->close(nullptr);
            return false;
        }
        impl_->output_stream->codecpar->codec_tag = 0;
        if (std::string(impl_->format->oformat->name).find("mp4") != std::string::npos ||
            std::string(impl_->format->oformat->name).find("mov") != std::string::npos) {
            impl_->output_stream->codecpar->codec_tag =
                mp4_codec_tag(impl_->output_stream->codecpar->codec_id);
        }
        impl_->output_stream->time_base = stream.impl_->time_base;
    } else {
        impl_->transcoder = std::make_unique<PacketTranscoder>();
        if (!impl_->transcoder->open(impl_->format, stream, options, error)) {
            impl_->close(nullptr);
            return false;
        }
        impl_->output_stream = impl_->transcoder->output_stream;
        if (std::string(impl_->format->oformat->name).find("mp4") != std::string::npos ||
            std::string(impl_->format->oformat->name).find("mov") != std::string::npos) {
            impl_->output_stream->codecpar->codec_tag = mp4_codec_tag(output_codec);
        }
    }
    if ((impl_->format->oformat->flags & AVFMT_NOFILE) == 0) {
        result = avio_open(&impl_->format->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (result < 0) {
            set_error(error, "cannot open movie file: " + ff_error(result));
            impl_->close(nullptr);
            return false;
        }
    }
    result = avformat_write_header(impl_->format, nullptr);
    if (result < 0) {
        set_error(error, "cannot write movie header: " + ff_error(result));
        impl_->close(nullptr);
        return false;
    }
    impl_->header_written = true;
    impl_->source = stream;
    impl_->timestamps.reset(av_rescale_q(1, AVRational{1, 1}, impl_->output_stream->time_base));
    return true;
}
bool EventMovieWriter::write_preroll(const PacketRing& ring, std::string* error) {
    for (const auto& packet : ring.snapshot_from_latest_keyframe()) {
        if (!impl_->write(packet, error)) {
            return false;
        }
    }
    return true;
}
bool EventMovieWriter::write(const VideoPacket& packet, std::string* error) {
    return impl_->write(packet, error);
}
bool EventMovieWriter::close(std::string* error) noexcept {
    return impl_->close(error);
}
bool EventMovieWriter::is_open() const noexcept {
    return impl_ && impl_->format != nullptr;
}

struct FragmentedMp4Writer::Impl {
    AVFormatContext* format = nullptr;
    AVStream* output_stream = nullptr;
    AVIOContext* io = nullptr;
    StreamInfo source;
    WriteCallback output;
    bool header_written = false;
    bool wrote_packet = false;
    bool flush_each_packet = false;
    detail::PacketTimestampNormalizer timestamps;
    std::unique_ptr<PacketTranscoder> transcoder;

    ~Impl() {
        close(nullptr);
    }

    static int write_bytes(void* opaque,
#if LIBAVFORMAT_VERSION_MAJOR >= 61
                           const std::uint8_t* buffer,
#else
                           std::uint8_t* buffer,
#endif
                           int size) noexcept {
        auto* self = static_cast<Impl*>(opaque);
        try {
            return self->output && self->output(buffer, static_cast<std::size_t>(size))
                       ? size
                       : AVERROR(EIO);
        } catch (...) {
            return AVERROR(EIO);
        }
    }

    bool write(const VideoPacket& input, std::string* error) {
        if (format == nullptr || !input.valid())
            return format != nullptr;
        if (transcoder) {
            const bool success = transcoder->write(input, error);
            if (success && flush_each_packet && format->pb != nullptr)
                avio_flush(format->pb);
            return success;
        }
        if (!codec_matches(input.stream().impl_->parameters->codec_id, source.codec_name())) {
            set_error(error, "web stream packet codec changed");
            return false;
        }
        if (!wrote_packet && !input.keyframe())
            return true;
        auto packet = make_packet();
        if (!packet || av_packet_ref(packet.get(), input.impl_->packet.get()) < 0) {
            set_error(error, "cannot clone web stream packet");
            return false;
        }
        const std::int64_t source_pts_delta =
            packet->pts != AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE
                ? packet->pts - packet->dts
                : 0;
        av_packet_rescale_ts(packet.get(), source.impl_->time_base, output_stream->time_base);
        const auto input_dts =
            packet->dts != AV_NOPTS_VALUE
                ? std::optional<std::int64_t>{packet->dts}
                : (packet->pts != AV_NOPTS_VALUE ? std::optional<std::int64_t>{packet->pts}
                                                 : std::nullopt);
        const auto arrival_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    input.received_at().time_since_epoch())
                                    .count();
        const auto arrival_timestamp =
            av_rescale_q(arrival_us, AVRational{1, 1000000}, output_stream->time_base);
        packet->dts = timestamps.normalize(input_dts, arrival_timestamp);
        const auto pts_delta =
            source_pts_delta > 0
                ? av_rescale_q(source_pts_delta, source.impl_->time_base, output_stream->time_base)
                : 0;
        packet->pts = packet->dts + std::max<std::int64_t>(pts_delta, 0);
        packet->stream_index = output_stream->index;
        packet->pos = -1;
        const int result = av_interleaved_write_frame(format, packet.get());
        if (result < 0) {
            set_error(error, "cannot write fragmented MP4 packet: " + ff_error(result));
            return false;
        }
        if (flush_each_packet && format->pb != nullptr)
            avio_flush(format->pb);
        wrote_packet = true;
        return true;
    }

    bool close(std::string* error) noexcept {
        bool success = true;
        if (transcoder && header_written) {
            try {
                success = transcoder->flush(error);
            } catch (...) {
                success = false;
            }
        }
        if (format != nullptr && header_written) {
            const int result = av_write_trailer(format);
            if (result < 0) {
                success = false;
                try {
                    set_error(error, "cannot finalize fragmented MP4: " + ff_error(result));
                } catch (...) {
                }
            }
        }
        if (format != nullptr)
            avformat_free_context(format);
        format = nullptr;
        output_stream = nullptr;
        if (io != nullptr) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        source = StreamInfo{};
        output = {};
        header_written = false;
        wrote_packet = false;
        flush_each_packet = false;
        timestamps.reset(1);
        transcoder.reset();
        return success;
    }
};

FragmentedMp4Writer::FragmentedMp4Writer() : impl_(std::make_unique<Impl>()) {
    configure_ffmpeg_logging();
}
FragmentedMp4Writer::~FragmentedMp4Writer() = default;
FragmentedMp4Writer::FragmentedMp4Writer(FragmentedMp4Writer&&) noexcept = default;
FragmentedMp4Writer& FragmentedMp4Writer::operator=(FragmentedMp4Writer&&) noexcept = default;
bool FragmentedMp4Writer::open(const StreamInfo& stream, const VideoEncodeOptions& options,
                               WriteCallback output, std::string* error) {
    impl_->close(nullptr);
    if (!stream.valid() || !output) {
        set_error(error, "camera stream or web output is unavailable");
        return false;
    }
    const std::string codec = normalize_video_codec(options.codec);
    const AVCodecID output_codec =
        codec == "copy" ? stream.impl_->parameters->codec_id : codec_id_for_name(codec);
    if (output_codec != AV_CODEC_ID_H264 && output_codec != AV_CODEC_ID_HEVC) {
        set_error(error, "fragmented MP4 webstream requires H.264 or HEVC output");
        return false;
    }
    int result = avformat_alloc_output_context2(&impl_->format, nullptr, "mp4", nullptr);
    if (result < 0 || impl_->format == nullptr) {
        set_error(error, "cannot allocate fragmented MP4 muxer: " + ff_error(result));
        return false;
    }
    auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
    impl_->io = buffer != nullptr
                    ? avio_alloc_context(buffer, 32768, 1, impl_.get(), nullptr,
                                         &FragmentedMp4Writer::Impl::write_bytes, nullptr)
                    : nullptr;
    if (impl_->io == nullptr) {
        set_error(error, "cannot allocate fragmented MP4 I/O");
        impl_->close(nullptr);
        return false;
    }
    impl_->format->pb = impl_->io;
    impl_->format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_FLUSH_PACKETS;
    impl_->flush_each_packet = options.fragment_every_frame;
    if (codec == "copy") {
        impl_->output_stream = avformat_new_stream(impl_->format, nullptr);
        result =
            impl_->output_stream == nullptr
                ? AVERROR(ENOMEM)
                : avcodec_parameters_copy(impl_->output_stream->codecpar, stream.impl_->parameters);
        if (result >= 0) {
            impl_->output_stream->codecpar->codec_tag =
                mp4_codec_tag(impl_->output_stream->codecpar->codec_id);
            impl_->output_stream->time_base = stream.impl_->time_base;
        }
    } else {
        impl_->transcoder = std::make_unique<PacketTranscoder>();
        result =
            impl_->transcoder->open(impl_->format, stream, options, error) ? 0 : AVERROR(EINVAL);
        if (result >= 0)
            impl_->output_stream = impl_->transcoder->output_stream;
    }
    if (result >= 0) {
        impl_->output_stream->codecpar->codec_tag =
            mp4_codec_tag(impl_->output_stream->codecpar->codec_id);
    }
    AVDictionary* muxer_options = nullptr;
    av_dict_set(&muxer_options, "movflags",
                options.fragment_every_frame ? "frag_every_frame+empty_moov+default_base_moof"
                                             : "frag_keyframe+empty_moov+default_base_moof",
                0);
    impl_->output = std::move(output);
    if (result >= 0)
        result = avformat_write_header(impl_->format, &muxer_options);
    av_dict_free(&muxer_options);
    if (result < 0) {
        if (error == nullptr || error->empty())
            set_error(error, "cannot initialize fragmented MP4 stream: " + ff_error(result));
        impl_->close(nullptr);
        return false;
    }
    impl_->source = stream;
    impl_->header_written = true;
    impl_->timestamps.reset(av_rescale_q(1, AVRational{1, 1}, impl_->output_stream->time_base),
                            impl_->source.impl_->repair_timestamps_from_arrival);
    return true;
}
bool FragmentedMp4Writer::write(const VideoPacket& packet, std::string* error) {
    return impl_->write(packet, error);
}
bool FragmentedMp4Writer::close(std::string* error) noexcept {
    return impl_->close(error);
}
bool FragmentedMp4Writer::is_open() const noexcept {
    return impl_ && impl_->format != nullptr;
}

struct TimelapseWriter::Impl {
    AVFormatContext* format = nullptr;
    AVStream* stream = nullptr;
    CodecPtr encoder;
    FramePtr encoded_frame;
    SwsContext* scaler = nullptr;
    StreamInfo packet_stream;
    TimelapseWriter::PacketCallback packet_callback;
    std::int64_t next_pts = 0;
    bool header_written = false;

    ~Impl() {
        close(nullptr);
    }

    bool drain(std::string* error) {
        auto packet = make_packet();
        if (!packet) {
            set_error(error, "cannot allocate timelapse packet");
            return false;
        }
        for (;;) {
            const int result = avcodec_receive_packet(encoder.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return true;
            }
            if (result < 0) {
                set_error(error, "cannot encode timelapse frame: " + ff_error(result));
                return false;
            }
            if (packet->duration <= 0) {
                packet->duration = 1;
            }
            VideoPacket published;
            if (packet_callback && packet_stream.valid()) {
                auto copy = std::make_unique<VideoPacket::Impl>();
                if (copy->packet && av_packet_ref(copy->packet.get(), packet.get()) >= 0) {
                    copy->stream = packet_stream;
                    copy->received = std::chrono::steady_clock::now();
                    published = VideoPacket(std::move(copy));
                }
            }
            av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int write_result = av_interleaved_write_frame(format, packet.get());
            av_packet_unref(packet.get());
            if (write_result < 0) {
                set_error(error, "cannot write timelapse packet: " + ff_error(write_result));
                return false;
            }
            if (published.valid()) {
                try {
                    packet_callback(published);
                } catch (...) {
                    // A web client must never interrupt the on-disk timelapse.
                }
            }
        }
    }

    bool close(std::string* error) noexcept {
        bool success = true;
        if (format != nullptr) {
            if (encoder && header_written) {
                int result = avcodec_send_frame(encoder.get(), nullptr);
                if (result >= 0) {
                    try {
                        success = drain(error) && success;
                    } catch (...) {
                        success = false;
                    }
                }
                result = av_write_trailer(format);
                if (result < 0) {
                    success = false;
                    try {
                        set_error(error, "cannot finalize timelapse: " + ff_error(result));
                    } catch (...) {
                    }
                }
            }
            if ((format->oformat->flags & AVFMT_NOFILE) == 0 && format->pb != nullptr) {
                avio_closep(&format->pb);
            }
            avformat_free_context(format);
        }
        sws_freeContext(scaler);
        scaler = nullptr;
        encoded_frame.reset();
        encoder.reset();
        format = nullptr;
        stream = nullptr;
        packet_stream = {};
        next_pts = 0;
        header_written = false;
        return success;
    }
};

TimelapseWriter::TimelapseWriter() : impl_(std::make_unique<Impl>()) {
    configure_ffmpeg_logging();
}
TimelapseWriter::~TimelapseWriter() = default;
TimelapseWriter::TimelapseWriter(TimelapseWriter&&) noexcept = default;
TimelapseWriter& TimelapseWriter::operator=(TimelapseWriter&&) noexcept = default;
bool TimelapseWriter::open(const std::string& path, int width, int height, int fps,
                           std::string* error) {
    TimelapseEncodeOptions options;
    options.codec = "mpeg4";
    return open(path, width, height, fps, options, error);
}
bool TimelapseWriter::open(const std::string& path, int width, int height, int fps,
                           const TimelapseEncodeOptions& options, std::string* error) {
    impl_->close(nullptr);
    if (width <= 0 || height <= 0 || fps <= 0) {
        set_error(error, "invalid timelapse dimensions or frame rate");
        return false;
    }
    if (options.quality < 0 || options.quality > 100 || options.bitrate < 0 ||
        options.keyframe_interval <= 0) {
        set_error(error, "invalid timelapse encoding options");
        return false;
    }
    if (!video_encoder_available(options.codec, options.encoder)) {
        set_error(error, "requested timelapse encoder is unavailable or cannot be opened");
        return false;
    }
    int result = avformat_alloc_output_context2(&impl_->format, nullptr, nullptr, path.c_str());
    if (result < 0 || impl_->format == nullptr) {
        set_error(error, "cannot select timelapse container: " + ff_error(result));
        return false;
    }
    const AVCodec* codec = find_requested_encoder(options.codec, options.encoder);
    impl_->stream = avformat_new_stream(impl_->format, nullptr);
    impl_->encoder.reset(codec ? avcodec_alloc_context3(codec) : nullptr);
    impl_->encoded_frame = make_frame();
    if (!codec || !impl_->stream || !impl_->encoder || !impl_->encoded_frame) {
        set_error(error, "requested timelapse encoder is unavailable or has the wrong codec");
        impl_->close(nullptr);
        return false;
    }
    impl_->encoder->width = width;
    impl_->encoder->height = height;
    impl_->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    impl_->encoder->time_base = AVRational{1, fps};
    impl_->encoder->framerate = AVRational{fps, 1};
    impl_->encoder->gop_size = std::max(fps * options.keyframe_interval, 1);
    impl_->encoder->keyint_min = impl_->encoder->gop_size;
    impl_->encoder->max_b_frames = 0;
    AVDictionary* codec_options = nullptr;
    if (std::string(codec->name) == "libx264") {
        // One hourly encoder remains open per camera. Bound x264's otherwise
        // large 4K frame-thread/lookahead queues before the first packet.
        impl_->encoder->thread_count = 1;
        av_dict_set(&codec_options, "preset", "veryfast", 0);
        av_dict_set(&codec_options, "tune", "zerolatency", 0);
        av_dict_set(&codec_options, "x264-params",
                    "threads=1:lookahead-threads=1:sync-lookahead=0:rc-lookahead=0:ref=1:"
                    "bframes=0:scenecut=0",
                    0);
    }
    if (options.quality > 0 && (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC)) {
        const std::string crf = std::to_string((100 - options.quality) * 51 / 100);
        av_dict_set(&codec_options, "crf", crf.c_str(), 0);
        if (std::string(codec->name) == "libx265")
            av_dict_set(&codec_options, "x265-params", "log-level=error", 0);
    } else if (options.quality > 0) {
        const int quantizer = 31 - ((options.quality - 1) * 29 / 99);
        impl_->encoder->flags |= AV_CODEC_FLAG_QSCALE;
        impl_->encoder->global_quality = FF_QP2LAMBDA * quantizer;
    } else if (options.bitrate > 0) {
        impl_->encoder->bit_rate = options.bitrate;
    } else {
        impl_->encoder->bit_rate =
            std::max<std::int64_t>(400000, static_cast<std::int64_t>(width) * height * fps / 5);
    }
    if ((impl_->format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        impl_->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    result = avcodec_open2(impl_->encoder.get(), codec, &codec_options);
    av_dict_free(&codec_options);
    if (result >= 0) {
        result = avcodec_parameters_from_context(impl_->stream->codecpar, impl_->encoder.get());
    }
    if (result >= 0) {
        auto packet_stream = std::make_shared<StreamInfo::Impl>();
        result = packet_stream->parameters == nullptr
                     ? AVERROR(ENOMEM)
                     : avcodec_parameters_copy(packet_stream->parameters, impl_->stream->codecpar);
        if (result >= 0) {
            packet_stream->time_base = impl_->encoder->time_base;
            packet_stream->frame_rate = impl_->encoder->framerate;
            packet_stream->repair_timestamps_from_arrival = false;
            impl_->packet_stream = StreamInfo(std::move(packet_stream));
        }
    }
    if (result < 0 || !allocate_video_frame(impl_->encoded_frame.get(), impl_->encoder->pix_fmt,
                                            width, height, error)) {
        if (result < 0) {
            set_error(error, "cannot initialize timelapse encoder: " + ff_error(result));
        }
        impl_->close(nullptr);
        return false;
    }
    impl_->stream->time_base = impl_->encoder->time_base;
    if (impl_->format->oformat != nullptr &&
        (std::string(impl_->format->oformat->name).find("mp4") != std::string::npos ||
         std::string(impl_->format->oformat->name).find("mov") != std::string::npos)) {
        impl_->stream->codecpar->codec_tag = mp4_codec_tag(codec->id);
    }
    if ((impl_->format->oformat->flags & AVFMT_NOFILE) == 0) {
        result = avio_open(&impl_->format->pb, path.c_str(), AVIO_FLAG_WRITE);
    }
    if (result >= 0) {
        result = avformat_write_header(impl_->format, nullptr);
    }
    if (result < 0) {
        set_error(error, "cannot open timelapse output: " + ff_error(result));
        impl_->close(nullptr);
        return false;
    }
    impl_->header_written = true;
    return true;
}

bool TimelapseWriter::write(const DecodedImage& image, std::string* error) {
    if (!is_open()) {
        set_error(error, "timelapse writer is not open");
        return false;
    }
    if (!image.valid()) {
        set_error(error, "invalid color timelapse frame");
        return false;
    }
    const AVFrame* source = image.impl_->frame.get();
    if (source->width <= 0 || source->height <= 0 || source->format < 0 ||
        source->data[0] == nullptr) {
        set_error(error, "invalid color timelapse frame");
        return false;
    }
    if (av_frame_make_writable(impl_->encoded_frame.get()) < 0) {
        set_error(error, "timelapse frame is not writable");
        return false;
    }
    impl_->scaler = sws_getCachedContext(
        impl_->scaler, source->width, source->height, static_cast<AVPixelFormat>(source->format),
        impl_->encoder->width, impl_->encoder->height, impl_->encoder->pix_fmt, SWS_BILINEAR,
        nullptr, nullptr, nullptr);
    if (!impl_->scaler) {
        set_error(error, "cannot create timelapse scaler");
        return false;
    }
    if (sws_scale(impl_->scaler, source->data, source->linesize, 0, source->height,
                  impl_->encoded_frame->data, impl_->encoded_frame->linesize) <= 0) {
        set_error(error, "cannot convert color timelapse frame");
        return false;
    }
    impl_->encoded_frame->pts = impl_->next_pts++;
    impl_->encoded_frame->quality = impl_->encoder->global_quality;
    const int result = avcodec_send_frame(impl_->encoder.get(), impl_->encoded_frame.get());
    if (result < 0) {
        set_error(error, "cannot submit timelapse frame: " + ff_error(result));
        return false;
    }
    return impl_->drain(error);
}
void TimelapseWriter::set_packet_callback(PacketCallback callback) {
    impl_->packet_callback = std::move(callback);
}
bool TimelapseWriter::close(std::string* error) noexcept {
    return impl_->close(error);
}
bool TimelapseWriter::is_open() const noexcept {
    return impl_ && impl_->format != nullptr;
}

} // namespace vibe_motion
