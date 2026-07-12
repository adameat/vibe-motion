#include "vibe_motion/media.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vibe_motion {
namespace {

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

struct StreamInfo::Impl {
    AVCodecParameters* parameters = nullptr;
    AVRational time_base{0, 1};

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
    int video_stream = -1;
    int output_width = 0;
    int output_height = 0;
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
        jpeg_sequence = 0;
        input_eof = false;
        stream = StreamInfo{};
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

    std::optional<GrayFrame> convert_gray(const AVFrame* source) {
        GrayFrame result;
        result.width = output_width;
        result.height = output_height;
        result.sequence = ++sequence;
        result.captured_at = std::chrono::system_clock::now();
        result.pixels.resize(static_cast<std::size_t>(output_width) *
                             static_cast<std::size_t>(output_height));
        std::uint8_t* destination[] = {result.pixels.data(), nullptr, nullptr, nullptr};
        int lines[] = {output_width, 0, 0, 0};
        gray_scaler = sws_getCachedContext(
            gray_scaler, source->width, source->height, static_cast<AVPixelFormat>(source->format),
            output_width, output_height, AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (gray_scaler == nullptr || sws_scale(gray_scaler, source->data, source->linesize, 0,
                                                source->height, destination, lines) <= 0) {
            return std::nullopt;
        }
        return result;
    }
};

NetworkCameraSource::NetworkCameraSource(CameraSourceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
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

CameraReadResult NetworkCameraSource::read() {
    if (!is_open()) {
        return {CameraReadStatus::error, std::nullopt, "camera is not open"};
    }
    auto decoded_sample = [this]() -> CameraReadResult {
        CameraSample sample;
        sample.frame = impl_->convert_gray(impl_->decoded.get());
        if (sample.frame) {
            auto retained = std::make_unique<DecodedImage::Impl>();
            if (!retained->frame || av_frame_ref(retained->frame.get(), impl_->decoded.get()) < 0) {
                return {CameraReadStatus::error, std::nullopt,
                        "cannot retain decoded camera frame"};
            }
            sample.image = DecodedImage(std::move(retained));
            sample.jpeg = impl_->encode_jpeg(impl_->decoded.get());
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
            sample.frame = impl_->convert_gray(impl_->decoded.get());
            if (sample.frame) {
                auto retained = std::make_unique<DecodedImage::Impl>();
                if (!retained->frame ||
                    av_frame_ref(retained->frame.get(), impl_->decoded.get()) < 0) {
                    return {CameraReadStatus::error, std::nullopt,
                            "cannot retain decoded camera frame"};
                }
                sample.image = DecodedImage(std::move(retained));
                sample.jpeg = impl_->encode_jpeg(impl_->decoded.get());
            }
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

struct EventMovieWriter::Impl {
    AVFormatContext* format = nullptr;
    AVStream* output_stream = nullptr;
    StreamInfo source;
    bool header_written = false;
    bool timestamp_base_set = false;
    std::int64_t base_dts = AV_NOPTS_VALUE;
    std::int64_t last_dts = AV_NOPTS_VALUE;

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
        auto packet = make_packet();
        if (!packet || av_packet_ref(packet.get(), input.impl_->packet.get()) < 0) {
            set_error(error, "cannot clone movie packet");
            return false;
        }
        if (!timestamp_base_set) {
            base_dts = packet->dts;
            if (base_dts == AV_NOPTS_VALUE) {
                base_dts = packet->pts;
            }
            timestamp_base_set = true;
        }
        if (packet->dts != AV_NOPTS_VALUE && base_dts != AV_NOPTS_VALUE) {
            packet->dts -= base_dts;
        }
        if (packet->pts != AV_NOPTS_VALUE && base_dts != AV_NOPTS_VALUE) {
            packet->pts -= base_dts;
        }
        av_packet_rescale_ts(packet.get(), source.impl_->time_base, output_stream->time_base);
        if (packet->dts == AV_NOPTS_VALUE) {
            packet->dts = last_dts == AV_NOPTS_VALUE ? 0 : last_dts + 1;
        }
        if (last_dts != AV_NOPTS_VALUE && packet->dts <= last_dts) {
            packet->dts = last_dts + 1;
        }
        if (packet->pts == AV_NOPTS_VALUE || packet->pts < packet->dts) {
            packet->pts = packet->dts;
        }
        last_dts = packet->dts;
        packet->stream_index = output_stream->index;
        packet->pos = -1;
        const int result = av_interleaved_write_frame(format, packet.get());
        if (result < 0) {
            set_error(error, "cannot write movie packet: " + ff_error(result));
            return false;
        }
        return true;
    }

    bool close(std::string* error) noexcept {
        bool success = true;
        if (format != nullptr) {
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
        timestamp_base_set = false;
        base_dts = last_dts = AV_NOPTS_VALUE;
        return success;
    }
};

EventMovieWriter::EventMovieWriter() : impl_(std::make_unique<Impl>()) {}
EventMovieWriter::~EventMovieWriter() = default;
EventMovieWriter::EventMovieWriter(EventMovieWriter&&) noexcept = default;
EventMovieWriter& EventMovieWriter::operator=(EventMovieWriter&&) noexcept = default;
bool EventMovieWriter::open(const std::string& path, const StreamInfo& stream, std::string* error) {
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
    impl_->output_stream->time_base = stream.impl_->time_base;
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

struct TimelapseWriter::Impl {
    AVFormatContext* format = nullptr;
    AVStream* stream = nullptr;
    CodecPtr encoder;
    FramePtr encoded_frame;
    SwsContext* scaler = nullptr;
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
            av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            const int write_result = av_interleaved_write_frame(format, packet.get());
            av_packet_unref(packet.get());
            if (write_result < 0) {
                set_error(error, "cannot write timelapse packet: " + ff_error(write_result));
                return false;
            }
        }
    }

    bool close(std::string* error) noexcept {
        bool success = true;
        if (format != nullptr) {
            if (encoder && header_written) {
                // MPEG-4/MP4 derives the final sample duration from the next
                // timestamp. Submit a hold frame as an end marker; the muxer
                // marks that sentinel disposable while retaining every frame
                // supplied by the caller.
                int result = 0;
                if (next_pts > 0 && encoded_frame) {
                    encoded_frame->pts = next_pts;
                    result = avcodec_send_frame(encoder.get(), encoded_frame.get());
                    if (result >= 0) {
                        try {
                            success = drain(error) && success;
                        } catch (...) {
                            success = false;
                        }
                    }
                }
                result = avcodec_send_frame(encoder.get(), nullptr);
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
        next_pts = 0;
        header_written = false;
        return success;
    }
};

TimelapseWriter::TimelapseWriter() : impl_(std::make_unique<Impl>()) {}
TimelapseWriter::~TimelapseWriter() = default;
TimelapseWriter::TimelapseWriter(TimelapseWriter&&) noexcept = default;
TimelapseWriter& TimelapseWriter::operator=(TimelapseWriter&&) noexcept = default;
bool TimelapseWriter::open(const std::string& path, int width, int height, int fps,
                           std::string* error) {
    impl_->close(nullptr);
    if (width <= 0 || height <= 0 || fps <= 0) {
        set_error(error, "invalid timelapse dimensions or frame rate");
        return false;
    }
    int result = avformat_alloc_output_context2(&impl_->format, nullptr, nullptr, path.c_str());
    if (result < 0 || impl_->format == nullptr) {
        set_error(error, "cannot select timelapse container: " + ff_error(result));
        return false;
    }
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    impl_->stream = avformat_new_stream(impl_->format, nullptr);
    impl_->encoder.reset(codec ? avcodec_alloc_context3(codec) : nullptr);
    impl_->encoded_frame = make_frame();
    if (!codec || !impl_->stream || !impl_->encoder || !impl_->encoded_frame) {
        set_error(error, "MPEG-4 timelapse encoder is unavailable");
        impl_->close(nullptr);
        return false;
    }
    impl_->encoder->width = width;
    impl_->encoder->height = height;
    impl_->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    impl_->encoder->time_base = AVRational{1, fps};
    impl_->encoder->framerate = AVRational{fps, 1};
    impl_->encoder->gop_size = std::max(fps * 10, 1);
    impl_->encoder->max_b_frames = 0;
    impl_->encoder->bit_rate =
        std::max<std::int64_t>(400000, static_cast<std::int64_t>(width) * height * fps / 5);
    if ((impl_->format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        impl_->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    result = avcodec_open2(impl_->encoder.get(), codec, nullptr);
    if (result >= 0) {
        result = avcodec_parameters_from_context(impl_->stream->codecpar, impl_->encoder.get());
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

bool TimelapseWriter::write(const GrayFrame& frame, std::string* error) {
    if (!is_open()) {
        set_error(error, "timelapse writer is not open");
        return false;
    }
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.pixels.size() <
            static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height)) {
        set_error(error, "invalid grayscale timelapse frame");
        return false;
    }
    if (av_frame_make_writable(impl_->encoded_frame.get()) < 0) {
        set_error(error, "timelapse frame is not writable");
        return false;
    }
    impl_->scaler = sws_getCachedContext(
        impl_->scaler, frame.width, frame.height, AV_PIX_FMT_GRAY8, impl_->encoder->width,
        impl_->encoder->height, impl_->encoder->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->scaler) {
        set_error(error, "cannot create timelapse scaler");
        return false;
    }
    const std::uint8_t* source[] = {frame.pixels.data(), nullptr, nullptr, nullptr};
    int source_lines[] = {frame.width, 0, 0, 0};
    sws_scale(impl_->scaler, source, source_lines, 0, frame.height, impl_->encoded_frame->data,
              impl_->encoded_frame->linesize);
    impl_->encoded_frame->pts = impl_->next_pts++;
    const int result = avcodec_send_frame(impl_->encoder.get(), impl_->encoded_frame.get());
    if (result < 0) {
        set_error(error, "cannot submit timelapse frame: " + ff_error(result));
        return false;
    }
    return impl_->drain(error);
}
bool TimelapseWriter::close(std::string* error) noexcept {
    return impl_->close(error);
}
bool TimelapseWriter::is_open() const noexcept {
    return impl_ && impl_->format != nullptr;
}

} // namespace vibe_motion
