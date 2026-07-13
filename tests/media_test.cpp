#include "vibe_motion/media.hpp"

#include "media_internal.hpp"

extern "C" {
#include <libavutil/frame.h>
}

#include <cassert>
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

static DecodedVideoStats decoded_video_stats(const std::filesystem::path& path) {
    CameraSourceConfig config;
    config.url = path.string();
    config.width = 160;
    config.height = 120;
    NetworkCameraSource source(config);
    std::string error;
    assert(source.open(&error));
    DecodedVideoStats stats;
    for (;;) {
        auto result = source.read();
        if (result.status == CameraReadStatus::end_of_stream)
            break;
        if (result.status == CameraReadStatus::again)
            continue;
        assert(result.status == CameraReadStatus::sample);
        assert(result.sample);
        if (result.sample->frame) {
            ++stats.frames;
            assert(result.sample->image);
            stats.has_color = stats.has_color || result.sample->image->has_color();
        }
    }
    source.close();
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
