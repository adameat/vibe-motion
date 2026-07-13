#include "vibe_motion/media.hpp"

#include "media_internal.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace vibe_motion;

static void test_decoded_frame_quality() {
    AVFrame frame{};
    assert(detail::decoded_frame_usable(&frame));
    assert(!detail::decoded_frame_usable(nullptr));

    frame.flags = AV_FRAME_FLAG_CORRUPT;
    assert(!detail::decoded_frame_usable(&frame));
    frame.flags = AV_FRAME_FLAG_DISCARD;
    assert(!detail::decoded_frame_usable(&frame));
    frame.flags = AV_FRAME_FLAG_KEY;
    assert(detail::decoded_frame_usable(&frame));

    frame.decode_error_flags = FF_DECODE_ERROR_INVALID_BITSTREAM;
    assert(!detail::decoded_frame_usable(&frame));
    frame.decode_error_flags = FF_DECODE_ERROR_MISSING_REFERENCE;
    assert(!detail::decoded_frame_usable(&frame));
    frame.decode_error_flags = FF_DECODE_ERROR_CONCEALMENT_ACTIVE;
    assert(!detail::decoded_frame_usable(&frame));
    frame.decode_error_flags = FF_DECODE_ERROR_DECODE_SLICES;
    assert(!detail::decoded_frame_usable(&frame));
}

struct DecodedVideoStats {
    int frames = 0;
    bool has_color = false;
};

static bool frame_has_color(const AVFrame* frame) {
    assert(frame != nullptr);
    const auto pixel_format = static_cast<AVPixelFormat>(frame->format);
    assert(pixel_format == AV_PIX_FMT_YUV420P || pixel_format == AV_PIX_FMT_YUVJ420P);
    assert(frame->data[1] != nullptr && frame->data[2] != nullptr);

    const int chroma_width = (frame->width + 1) / 2;
    const int chroma_height = (frame->height + 1) / 2;
    for (int plane = 1; plane <= 2; ++plane) {
        for (int row = 0; row < chroma_height; ++row) {
            const auto* pixels =
                frame->data[plane] + static_cast<std::ptrdiff_t>(row) * frame->linesize[plane];
            if (std::any_of(pixels, pixels + chroma_width, [](std::uint8_t value) {
                    constexpr int neutral = 128;
                    constexpr int tolerance = 8;
                    const int chroma = value;
                    return chroma < neutral - tolerance || chroma > neutral + tolerance;
                })) {
                return true;
            }
        }
    }
    return false;
}

static void receive_frames(AVCodecContext* decoder, AVFrame* frame, DecodedVideoStats& stats) {
    for (;;) {
        const int result = avcodec_receive_frame(decoder, frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return;
        }
        assert(result >= 0);
        ++stats.frames;
        stats.has_color = stats.has_color || frame_has_color(frame);
        av_frame_unref(frame);
    }
}

static DecodedVideoStats decoded_video_stats(const std::filesystem::path& path) {
    AVFormatContext* format = nullptr;
    const std::string path_text = path.string();
    assert(avformat_open_input(&format, path_text.c_str(), nullptr, nullptr) >= 0);
    assert(avformat_find_stream_info(format, nullptr) >= 0);

    int video_stream = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = static_cast<int>(index);
            break;
        }
    }
    assert(video_stream >= 0);
    AVStream* stream = format->streams[static_cast<unsigned int>(video_stream)];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* decoder = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
    assert(decoder != nullptr);
    assert(avcodec_parameters_to_context(decoder, stream->codecpar) >= 0);
    assert(avcodec_open2(decoder, codec, nullptr) >= 0);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    assert(packet != nullptr && frame != nullptr);
    DecodedVideoStats stats;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video_stream) {
            assert(avcodec_send_packet(decoder, packet) >= 0);
            receive_frames(decoder, frame, stats);
        }
        av_packet_unref(packet);
    }
    assert(avcodec_send_packet(decoder, nullptr) >= 0);
    receive_frames(decoder, frame, stats);

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return stats;
}

int main(int, char** argv) {
    test_decoded_frame_quality();
    const auto directory = std::filesystem::path(argv[0]).parent_path();
    const auto input = directory / "media-fixture.mp4";
    if (!std::filesystem::exists(input)) {
        std::cout << "media fixture absent; test skipped\n";
        return 0;
    }
    const auto movie_path = directory / "media-event.mp4";
    std::filesystem::remove(movie_path);

    CameraSourceConfig config;
    config.url = input.string();
    config.width = 160;
    config.height = 120;
    config.jpeg_quality = 80;
    NetworkCameraSource source(config);
    std::string error;
    assert(source.open(&error));
    assert(source.stream_info().valid());

    PacketRing ring(std::chrono::seconds(10), 1000);
    EventMovieWriter movie;
    std::vector<DecodedImage> images;
    constexpr std::size_t timelapse_frame_limit = 5;
    bool movie_opened = false;
    bool overlay_tested = false;
    int samples = 0;
    for (;;) {
        auto result = source.read();
        if (result.status == CameraReadStatus::end_of_stream)
            break;
        if (result.status == CameraReadStatus::again)
            continue;
        assert(result.status == CameraReadStatus::sample);
        assert(result.sample.has_value());
        auto& sample = *result.sample;
        ring.push(sample.packet);
        if (sample.frame) {
            assert(sample.frame->pixels.size() == 160U * 120U);
            assert(sample.image);
            const auto jpeg = source.render_jpeg(*sample.image);
            assert(jpeg.size() > 4);
            assert(jpeg[0] == 0xff && jpeg[1] == 0xd8);
            if (images.size() < timelapse_frame_limit) {
                images.push_back(*sample.image);
            }
            if (!overlay_tested && sample.image) {
                const auto annotated =
                    source.render_jpeg(*sample.image, RedBox{20, 20, 100, 80, 4});
                assert(annotated.size() > 4);
                assert(annotated != jpeg);
                std::ofstream output(directory / "media-redbox.jpg", std::ios::binary);
                output.write(reinterpret_cast<const char*>(annotated.data()),
                             static_cast<std::streamsize>(annotated.size()));
                assert(output.good());
                overlay_tested = true;
            }
            ++samples;
        }
        if (!movie_opened && samples >= 10) {
            assert(movie.open(movie_path.string(), source.stream_info(), &error));
            assert(movie.write_preroll(ring, &error));
            movie_opened = true;
        } else if (movie_opened) {
            assert(movie.write(sample.packet, &error));
        }
    }
    assert(samples >= 20);
    assert(overlay_tested);
    assert(movie.close(&error));
    assert(std::filesystem::file_size(movie_path) > 1000);

    assert(images.size() == timelapse_frame_limit);
    const auto timelapse_frames = images.size();
    for (const std::string extension : {".mkv", ".avi"}) {
        const auto timelapse_path = directory / ("media-timelapse" + extension);
        std::filesystem::remove(timelapse_path);
        TimelapseWriter timelapse;
        assert(timelapse.open(timelapse_path.string(), 160, 120, 1, &error));
        for (std::size_t index = 0; index < timelapse_frames; ++index) {
            assert(timelapse.write(images[index], &error));
        }
        assert(timelapse.close(&error));
        assert(std::filesystem::file_size(timelapse_path) > 1000);
        const auto stats = decoded_video_stats(timelapse_path);
        assert(stats.frames == static_cast<int>(timelapse_frames));
        assert(stats.has_color);
    }
    source.close();

    config.analysis_framerate = 2;
    NetworkCameraSource throttled(config);
    assert(throttled.open(&error));
    int analyzed = 0;
    int packets = 0;
    for (;;) {
        auto result = throttled.read();
        if (result.status == CameraReadStatus::end_of_stream)
            break;
        if (result.status == CameraReadStatus::again)
            continue;
        assert(result.status == CameraReadStatus::sample);
        assert(result.sample);
        analyzed += result.sample->frame != nullptr ? 1 : 0;
        packets += result.sample->packet.valid() ? 1 : 0;
    }
    assert(analyzed >= 5 && analyzed <= 7);
    assert(packets > analyzed);
    throttled.close();

    std::cout << "media tests passed\n";
}
