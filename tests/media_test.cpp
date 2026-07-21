#include "vibe_motion/media.hpp"

#include "media_internal.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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
#ifdef AV_FRAME_FLAG_KEY
    frame.flags = AV_FRAME_FLAG_KEY;
#else
    frame.flags = 0;
#endif
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
    int keyframes = 0;
    bool has_color = false;
    std::string codec;
    std::string codec_tag;
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
    stats.codec = avcodec_get_name(stream->codecpar->codec_id);
    if (stream->codecpar->codec_tag != 0) {
        for (unsigned int shift = 0; shift < 32; shift += 8)
            stats.codec_tag.push_back(
                static_cast<char>((stream->codecpar->codec_tag >> shift) & 0xffU));
    }
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == video_stream) {
            stats.keyframes += (packet->flags & AV_PKT_FLAG_KEY) != 0 ? 1 : 0;
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

static void test_hevc_outputs(const std::filesystem::path& directory) {
    const auto input =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "media-hevc-fixture.mp4";
    assert(std::filesystem::exists(input));
    const bool has_hevc_encoder = video_encoder_available("hevc");
    const bool has_h264_encoder = video_encoder_available("h264", "libx264");
    assert(!video_encoder_available("hevc", "libx264"));

    CameraSourceConfig config;
    config.url = input.string();
    config.width = 160;
    config.height = 120;
    config.jpeg_quality = 80;
    NetworkCameraSource source(config);
    std::string error;
    assert(source.open(&error));
    assert(source.stream_info().codec_name() == "hevc");

    const auto event_path = directory / "media-hevc-event.mp4";
    const auto fragmented_path = directory / "media-hevc-fragmented.mp4";
    const auto transcoded_event_path = directory / "media-hevc-transcoded-event.mp4";
    const auto transcoded_fragmented_path = directory / "media-hevc-transcoded-fragmented.mp4";
    const auto timelapse_path = directory / "media-hevc-timelapse.mkv";
    const auto h264_timelapse_path = directory / "media-h264-timelapse.mkv";
    std::filesystem::remove(event_path);
    std::filesystem::remove(fragmented_path);
    std::filesystem::remove(transcoded_event_path);
    std::filesystem::remove(transcoded_fragmented_path);
    std::filesystem::remove(timelapse_path);
    std::filesystem::remove(h264_timelapse_path);

    PacketRing ring(std::chrono::seconds(10), 1000);
    EventMovieWriter event;
    FragmentedMp4Writer fragmented;
    const VideoEncodeOptions copy_options{};
    std::vector<std::uint8_t> fragmented_bytes;
    assert(fragmented.open(
        source.stream_info(), copy_options,
        [&](const std::uint8_t* bytes, std::size_t size) {
            fragmented_bytes.insert(fragmented_bytes.end(), bytes, bytes + size);
            return true;
        },
        &error));
    std::vector<DecodedImage> images;
    int packets = 0;
    for (;;) {
        auto read = source.read();
        if (read.status == CameraReadStatus::end_of_stream)
            break;
        if (read.status == CameraReadStatus::again)
            continue;
        assert(read.status == CameraReadStatus::sample && read.sample);
        auto& sample = *read.sample;
        if (sample.packet.valid()) {
            ring.push(sample.packet);
            assert(fragmented.write(sample.packet, &error));
            ++packets;
            if (packets == 15) {
                assert(event.open(event_path.string(), source.stream_info(), copy_options, &error));
                assert(event.write_preroll(ring, &error));
            } else if (event.is_open()) {
                assert(event.write(sample.packet, &error));
            }
        }
        if (sample.image && images.empty()) {
            const auto jpeg = source.render_jpeg(*sample.image);
            assert(jpeg.size() > 4 && jpeg[0] == 0xff && jpeg[1] == 0xd8);
        }
        if (sample.image && images.size() < 5)
            images.push_back(*sample.image);
    }
    assert(packets >= 25);
    assert(event.close(&error));
    assert(fragmented.close(&error));
    source.close();

    std::ofstream fragmented_file(fragmented_path, std::ios::binary);
    fragmented_file.write(reinterpret_cast<const char*>(fragmented_bytes.data()),
                          static_cast<std::streamsize>(fragmented_bytes.size()));
    fragmented_file.close();
    assert(fragmented_file.good());

    const auto event_stats = decoded_video_stats(event_path);
    assert(event_stats.codec == "hevc");
    assert(event_stats.codec_tag == "hvc1");
    assert(event_stats.frames >= 15);
    assert(event_stats.keyframes >= 1);
    const auto fragmented_stats = decoded_video_stats(fragmented_path);
    assert(fragmented_stats.codec == "hevc");
    assert(fragmented_stats.codec_tag == "hvc1");
    assert(fragmented_stats.frames == packets);

    std::string transcode_codec;
    if (has_hevc_encoder)
        transcode_codec = "hevc";
    else if (video_encoder_available("h264"))
        transcode_codec = "h264";
    if (!transcode_codec.empty()) {
        const auto preroll = ring.snapshot_from_latest_keyframe();
        const VideoEncodeOptions transcode_options{
            .quality = 60,
            .bitrate = 0,
            .codec = transcode_codec,
            .encoder = {},
            .keyframe_interval = 1,
        };
        EventMovieWriter transcoded_event;
        assert(transcoded_event.open(transcoded_event_path.string(), preroll.front().stream(),
                                     transcode_options, &error));
        assert(transcoded_event.write_preroll(ring, &error));
        assert(transcoded_event.close(&error));
        const auto transcoded_event_stats = decoded_video_stats(transcoded_event_path);
        assert(transcoded_event_stats.codec == transcode_codec);
        assert(transcoded_event_stats.frames == static_cast<int>(preroll.size()));

        std::vector<std::uint8_t> transcoded_fragmented_bytes;
        FragmentedMp4Writer transcoded_fragmented;
        assert(transcoded_fragmented.open(
            preroll.front().stream(), transcode_options,
            [&](const std::uint8_t* bytes, std::size_t size) {
                transcoded_fragmented_bytes.insert(transcoded_fragmented_bytes.end(), bytes,
                                                   bytes + size);
                return true;
            },
            &error));
        for (const auto& packet : preroll)
            assert(transcoded_fragmented.write(packet, &error));
        assert(transcoded_fragmented.close(&error));
        std::ofstream transcoded_fragmented_file(transcoded_fragmented_path, std::ios::binary);
        transcoded_fragmented_file.write(
            reinterpret_cast<const char*>(transcoded_fragmented_bytes.data()),
            static_cast<std::streamsize>(transcoded_fragmented_bytes.size()));
        transcoded_fragmented_file.close();
        const auto transcoded_fragmented_stats = decoded_video_stats(transcoded_fragmented_path);
        assert(transcoded_fragmented_stats.codec == transcode_codec);
        assert(transcoded_fragmented_stats.frames == static_cast<int>(preroll.size()));
    }

    TimelapseWriter timelapse;
    const TimelapseEncodeOptions options{
        .quality = 60,
        .bitrate = 0,
        .codec = "hevc",
        .encoder = {},
        .keyframe_interval = 2,
    };
    if (!has_hevc_encoder) {
        assert(!timelapse.open(timelapse_path.string(), 160, 120, 1, options, &error));
        assert(error.find("unavailable") != std::string::npos);
        return;
    }
    assert(timelapse.open(timelapse_path.string(), 160, 120, 1, options, &error));
    for (const auto& image : images)
        assert(timelapse.write(image, &error));
    assert(timelapse.close(&error));
    const auto timelapse_stats = decoded_video_stats(timelapse_path);
    assert(timelapse_stats.codec == "hevc");
    assert(timelapse_stats.frames == static_cast<int>(images.size()));
    assert(timelapse_stats.has_color);

    if (has_h264_encoder) {
        TimelapseWriter h264_timelapse;
        std::vector<VideoPacket> h264_packets;
        h264_timelapse.set_packet_callback(
            [&](const VideoPacket& packet) { h264_packets.push_back(packet); });
        const TimelapseEncodeOptions h264_options{
            .quality = 55,
            .bitrate = 0,
            .codec = "h264",
            .encoder = "libx264",
            .keyframe_interval = 2,
        };
        assert(
            h264_timelapse.open(h264_timelapse_path.string(), 160, 120, 1, h264_options, &error));
        for (const auto& image : images)
            assert(h264_timelapse.write(image, &error));
        assert(h264_timelapse.close(&error));
        const auto h264_stats = decoded_video_stats(h264_timelapse_path);
        assert(h264_stats.codec == "h264");
        assert(h264_stats.frames == static_cast<int>(images.size()));
        assert(h264_stats.keyframes >= 2);
        assert(h264_stats.has_color);
        assert(h264_packets.size() == images.size());
        assert(h264_packets.front().stream().codec_name() == "h264");
        assert(h264_packets.front().keyframe());
        assert(std::count_if(h264_packets.begin(), h264_packets.end(),
                             [](const VideoPacket& packet) { return packet.keyframe(); }) >= 2);

        if (std::getenv("VIBE_X264_STRESS") != nullptr) {
            auto stress_options = h264_options;
            stress_options.keyframe_interval = 300;
            constexpr std::array<std::pair<int, int>, 7> dimensions{{
                {3840, 2160},
                {2560, 1920},
                {2560, 1920},
                {2560, 1920},
                {2560, 1920},
                {2560, 1920},
                {2560, 1920},
            }};
            std::vector<std::unique_ptr<TimelapseWriter>> writers;
            for (std::size_t index = 0; index < dimensions.size(); ++index) {
                const auto path =
                    directory / ("media-x264-stress-" + std::to_string(index) + ".mkv");
                std::filesystem::remove(path);
                auto writer = std::make_unique<TimelapseWriter>();
                assert(writer->open(path.string(), dimensions[index].first,
                                    dimensions[index].second, 1, stress_options, &error));
                writers.push_back(std::move(writer));
            }
            for (int frame = 0; frame < 320; ++frame) {
                for (auto& writer : writers)
                    assert(writer->write(images[static_cast<std::size_t>(frame) % images.size()],
                                         &error));
            }
            for (auto& writer : writers)
                assert(writer->close(&error));
        }
    }
}

int main(int, char** argv) {
    test_decoded_frame_quality();
    assert(normalize_video_codec("H265") == "hevc");
    assert(normalize_video_codec("libx264") == "h264");
    assert(normalize_video_codec("passthrough") == "copy");
    assert(normalize_video_codec("mpeg4") == "mpeg4");
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
        const TimelapseEncodeOptions options{
            .quality = 75,
            .bitrate = 0,
            .codec = "mpeg4",
            .encoder = {},
            .keyframe_interval = 2,
        };
        assert(timelapse.open(timelapse_path.string(), 160, 120, 1, options, &error));
        for (std::size_t index = 0; index < timelapse_frames; ++index) {
            assert(timelapse.write(images[index], &error));
        }
        assert(timelapse.close(&error));
        assert(std::filesystem::file_size(timelapse_path) > 1000);
        const auto stats = decoded_video_stats(timelapse_path);
        assert(stats.frames == static_cast<int>(timelapse_frames));
        assert(stats.keyframes >= 2);
        assert(stats.has_color);
    }
    source.close();

    test_hevc_outputs(directory);

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

    config.analysis_framerate = 0;
    config.decode_mode = FrameDecodeMode::keyframes;
    NetworkCameraSource keyframes_only(config);
    assert(keyframes_only.open(&error));
    int keyframe_images = 0;
    int retained_packets = 0;
    for (;;) {
        auto result = keyframes_only.read();
        if (result.status == CameraReadStatus::end_of_stream)
            break;
        if (result.status == CameraReadStatus::again)
            continue;
        assert(result.status == CameraReadStatus::sample);
        assert(result.sample);
        retained_packets += result.sample->packet.valid() ? 1 : 0;
        if (result.sample->frame) {
            assert(result.sample->decoded_keyframe);
            ++keyframe_images;
        }
    }
    assert(keyframe_images >= 2 && keyframe_images <= 4);
    assert(retained_packets >= 25);
    assert(retained_packets > keyframe_images);
    keyframes_only.close();

    NetworkCameraSource switching(config);
    assert(switching.open(&error));
    assert(switching.decode_mode() == FrameDecodeMode::keyframes);
    int switched_images = 0;
    bool switched = false;
    for (;;) {
        auto result = switching.read();
        if (result.status == CameraReadStatus::end_of_stream)
            break;
        if (result.status == CameraReadStatus::again)
            continue;
        assert(result.status == CameraReadStatus::sample);
        assert(result.sample);
        if (result.sample->frame) {
            ++switched_images;
        }
        if (!switched && result.sample->packet.keyframe()) {
            switching.set_decode_mode(FrameDecodeMode::all);
            switched = true;
        }
    }
    assert(switched);
    assert(switching.decode_mode() == FrameDecodeMode::all);
    assert(switched_images >= 20);
    switching.close();

    std::cout << "media tests passed\n";
}
