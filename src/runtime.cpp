#include "vibe_motion/runtime.hpp"

#include "vibe_motion/detection.hpp"
#include "vibe_motion/event.hpp"
#include "vibe_motion/hooks.hpp"
#include "vibe_motion/http.hpp"
#include "vibe_motion/log.hpp"
#include "vibe_motion/media.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vibe_motion {
namespace {

using namespace std::chrono_literals;

std::string redact_secrets(std::string text) {
    static const std::regex user_info(R"(([a-zA-Z][a-zA-Z0-9+.-]*://)[^/@\s]+@)");
    static const std::regex query_secret(R"(((?:user|username|password|pass|token)=)[^&\s]+)",
                                         std::regex::icase);
    text = std::regex_replace(text, user_info, "$1[REDACTED]@");
    return std::regex_replace(text, query_secret, "$1[REDACTED]");
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    return output.str();
}

std::filesystem::path ensure_extension(std::filesystem::path path, const std::string& extension) {
    if (!path.has_extension()) {
        path += extension;
    }
    return path;
}

void write_atomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(::getpid());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create " + path.string());
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("cannot write " + path.string());
        }
    }
    std::filesystem::rename(temporary, path);
}

void update_lastsnap(const std::filesystem::path& snapshot) {
    if (snapshot.filename().string().find("lastsnap") != std::string::npos) {
        return;
    }
    const auto link = snapshot.parent_path() / "lastsnap.jpg";
    auto temporary = link;
    temporary += ".tmp-" + std::to_string(::getpid());
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    std::filesystem::create_symlink(snapshot.filename(), temporary, ignored);
    if (ignored) {
        return;
    }
    std::filesystem::rename(temporary, link, ignored);
    if (ignored) {
        std::filesystem::remove(temporary, ignored);
    }
}

std::unordered_map<std::string, std::string> parse_netcam_options(const std::string& value) {
    std::unordered_map<std::string, std::string> result;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find(',', begin);
        const auto item =
            value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const auto equals = item.find('=');
        if (equals != std::string::npos) {
            result[item.substr(0, equals)] = item.substr(equals + 1);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

std::string hour_key(std::chrono::system_clock::time_point when) {
    const auto instant = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    localtime_r(&instant, &local);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d%H", &local);
    return buffer;
}

struct WorkerStatus {
    bool connected = false;
    bool event_active = false;
    std::uint64_t frames = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t changed_pixels = 0;
    std::uint64_t event_number = 0;
    std::string error;
};

class CameraWorker {
  public:
    CameraWorker(CameraConfig config, std::filesystem::path target_dir, HookExecutor& hooks,
                 HttpServer* http)
        : config_(std::move(config)), target_dir_(std::move(target_dir)), hooks_(hooks),
          http_(http) {}

    ~CameraWorker() {
        stop();
    }

    void start() {
        stopping_.store(false);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        request_stop();
        join();
    }

    void request_stop() noexcept {
        stopping_.store(true);
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    WorkerStatus status() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return status_;
    }

    const CameraConfig& config() const noexcept {
        return config_;
    }

  private:
    ExpansionContext context(const DetectionResult& detection, const GrayFrame& frame,
                             std::uint64_t event, std::string filename = {},
                             std::string type = {}) const {
        ExpansionContext result;
        result.camera_id = config_.camera_id;
        result.camera_name = config_.camera_name;
        result.event_number = event;
        result.threshold = static_cast<int>(
            std::min<std::uint64_t>(detection.effective_threshold,
                                    static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
        result.changed_pixels = static_cast<int>(std::min<std::uint64_t>(
            detection.changed_pixels, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
        result.noise_level = config_.noise_level;
        result.filename = std::move(filename);
        result.file_type = std::move(type);
        result.frame_number = static_cast<std::uint64_t>(frame.sequence);
        result.width = frame.width;
        result.height = frame.height;
        result.fps = config_.framerate;
        char hostname[256]{};
        if (::gethostname(hostname, sizeof(hostname) - 1) == 0) {
            result.host = hostname;
        }
        return result;
    }

    void hook(const std::string& command, const ExpansionContext& values,
              std::chrono::system_clock::time_point when) {
        if (command.empty()) {
            return;
        }
        try {
            auto argv = parse_hook_command(command);
            for (auto& argument : argv) {
                argument = expand_template(argument, values, when);
            }
            if (!hooks_.submit(std::move(argv))) {
                Logger::instance().write(LogLevel::warning, "camera ", config_.camera_id,
                                         ": hook queue full");
            }
        } catch (const std::exception& error) {
            Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                     ": invalid hook: ", error.what());
        }
    }

    void set_status(const std::function<void(WorkerStatus&)>& update) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        update(status_);
    }

    std::filesystem::path output_path(const std::string& pattern, const std::string& extension,
                                      const ExpansionContext& values,
                                      std::chrono::system_clock::time_point when) const {
        return ensure_extension(expand_path(pattern, target_dir_, values, when), extension);
    }

    void save_snapshot(const std::vector<std::uint8_t>& jpeg, const GrayFrame& frame,
                       const DetectionResult& detection, std::uint64_t event_number) {
        if (jpeg.empty()) {
            return;
        }
        auto values = context(detection, frame, event_number);
        const auto path = output_path(config_.snapshot_filename, ".jpg", values, frame.captured_at);
        write_atomic(path, jpeg);
        update_lastsnap(path);
        values.filename = path.string();
        values.file_type = "picture";
        hook(config_.on_picture_save, values, frame.captured_at);
    }

    void finish_event(EventMovieWriter& movie, std::filesystem::path& movie_path,
                      std::vector<std::uint8_t>& best_jpeg, GrayFrame& best_frame,
                      DetectionResult& best_detection, std::uint64_t event_number) {
        const auto picture_when = best_frame.captured_at.time_since_epoch().count() == 0
                                      ? std::chrono::system_clock::now()
                                      : best_frame.captured_at;
        const auto end_when = std::chrono::system_clock::now();
        auto values = context(best_detection, best_frame, event_number);
        if (config_.picture_output && !best_jpeg.empty()) {
            const auto picture =
                output_path(config_.picture_filename, ".jpg", values, picture_when);
            try {
                write_atomic(picture, best_jpeg);
                values.filename = picture.string();
                values.file_type = "picture";
                hook(config_.on_picture_save, values, picture_when);
            } catch (const std::exception& error) {
                Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                         ": picture write failed: ", error.what());
            }
        }
        values.filename.clear();
        values.file_type.clear();
        hook(config_.on_event_end, values, end_when);
        if (movie.is_open()) {
            std::string error;
            movie.close(&error);
            values.filename = movie_path.string();
            values.file_type = "movie";
            hook(config_.on_movie_end, values, end_when);
            if (!error.empty()) {
                Logger::instance().write(LogLevel::warning, "camera ", config_.camera_id,
                                         ": movie finalize: ", redact_secrets(error));
            }
        }
        movie_path.clear();
        best_jpeg.clear();
        best_frame = {};
        best_detection = {};
    }

    void run() {
        DetectionSettings detection_settings;
        detection_settings.threshold = static_cast<std::uint64_t>(config_.threshold);
        detection_settings.threshold_tune = config_.threshold_tune;
        detection_settings.noise_level =
            static_cast<std::uint8_t>(std::clamp(config_.noise_level, 0, 255));
        detection_settings.lightswitch_percent = config_.lightswitch_percent;
        detection_settings.despeckle = !config_.despeckle_filter.empty();
        MotionDetector detector(detection_settings);
        if (!config_.mask_file.empty()) {
            try {
                detector.load_pgm_mask(config_.mask_file, config_.width, config_.height);
            } catch (const std::exception& error) {
                Logger::instance().write(LogLevel::error, "camera ", config_.camera_id, ": ",
                                         error.what());
                set_status([&](WorkerStatus& state) { state.error = error.what(); });
                return;
            }
        }

        EventStateMachine events({config_.minimum_motion_frames,
                                  std::chrono::seconds(config_.event_gap), config_.post_capture});
        const double pre_seconds =
            static_cast<double>(config_.pre_capture + config_.minimum_motion_frames) /
            std::max(1, config_.framerate);
        PacketRing ring(
            std::chrono::milliseconds(static_cast<int>(std::max(5.0, pre_seconds + 2.0) * 1000.0)),
            8192);
        EventMovieWriter movie;
        TimelapseWriter timelapse;
        std::filesystem::path movie_path;
        std::string timelapse_hour;
        std::vector<std::uint8_t> best_jpeg;
        GrayFrame best_frame;
        DetectionResult best_detection;
        std::int64_t snapshot_bucket = -1;
        std::int64_t timelapse_bucket = -1;
        std::chrono::steady_clock::time_point last_http_publish{};
        bool record_live = false;

        while (!stopping_.load()) {
            CameraSourceConfig source_config;
            source_config.url = config_.netcam_url;
            source_config.width = config_.width;
            source_config.height = config_.height;
            source_config.jpeg_quality = config_.movie_quality;
            const auto net_options = parse_netcam_options(config_.netcam_params);
            if (const auto found = net_options.find("interrupt"); found != net_options.end()) {
                try {
                    source_config.io_timeout = std::chrono::seconds(std::stoi(found->second));
                } catch (...) { /* config validation owns diagnostics */
                }
            }
            if (config_.netcam_use_tcp || (net_options.count("rtsp_transport") &&
                                           net_options.at("rtsp_transport") == "tcp")) {
                source_config.rtsp_transport = "tcp";
            } else {
                source_config.rtsp_transport.clear();
            }
            for (const auto& [key, value] : net_options) {
                if (key != "interrupt" && key != "capture_rate" && key != "rtsp_transport") {
                    source_config.options[key] = value;
                }
            }

            NetworkCameraSource source(std::move(source_config));
            std::string open_error;
            if (!source.open(&open_error)) {
                set_status([&](WorkerStatus& state) {
                    state.connected = false;
                    ++state.reconnects;
                    state.error = redact_secrets(open_error);
                });
                Logger::instance().write(LogLevel::warning, "camera ", config_.camera_id,
                                         ": connect failed: ", redact_secrets(open_error));
                for (int tenth = 0; tenth < 10 && !stopping_.load(); ++tenth) {
                    std::this_thread::sleep_for(100ms);
                }
                continue;
            }
            set_status([](WorkerStatus& state) {
                state.connected = true;
                state.error.clear();
            });
            Logger::instance().write(LogLevel::info, "camera ", config_.camera_id, " connected (",
                                     source.stream_info().codec_name(), ")");
            const auto warm_until = std::chrono::steady_clock::now() + 2s;

            while (!stopping_.load()) {
                auto read = source.read();
                if (read.status == CameraReadStatus::again) {
                    continue;
                }
                if (read.status != CameraReadStatus::sample || !read.sample) {
                    set_status([&](WorkerStatus& state) {
                        state.connected = false;
                        ++state.reconnects;
                        state.error = redact_secrets(read.error);
                    });
                    Logger::instance().write(LogLevel::warning, "camera ", config_.camera_id,
                                             ": stream interrupted: ", redact_secrets(read.error));
                    break;
                }

                auto& sample = *read.sample;
                ring.push(sample.packet);
                if (!sample.frame) {
                    if (movie.is_open() && record_live) {
                        std::string error;
                        if (!movie.write(sample.packet, &error)) {
                            Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                                     ": movie write: ", redact_secrets(error));
                        }
                    }
                    continue;
                }
                auto& frame = *sample.frame;
                const auto detection = detector.process(frame);
                const bool warmed = std::chrono::steady_clock::now() >= warm_until;
                const auto decision =
                    events.update(warmed && detection.motion, std::chrono::steady_clock::now());
                if (movie.is_open() && !decision.event_started && decision.record_frame) {
                    std::string error;
                    if (!movie.write(sample.packet, &error)) {
                        Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                                 ": movie write: ", redact_secrets(error));
                    }
                }
                record_live = decision.record_frame;
                set_status([&](WorkerStatus& state) {
                    state.frames++;
                    state.changed_pixels = detection.changed_pixels;
                    state.event_active = events.active();
                    state.event_number = events.event_number();
                });

                std::vector<std::uint8_t> plain_jpeg;
                bool jpeg_attempted = false;
                const auto ensure_jpeg = [&]() -> const std::vector<std::uint8_t>& {
                    if (!jpeg_attempted) {
                        jpeg_attempted = true;
                        if (sample.image) {
                            plain_jpeg = source.render_jpeg(*sample.image);
                        }
                    }
                    return plain_jpeg;
                };

                const auto epoch_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                               frame.captured_at.time_since_epoch())
                                               .count();
                bool snapshot_due = false;
                if (config_.snapshot_interval > 0) {
                    const auto bucket = epoch_seconds / config_.snapshot_interval;
                    if (bucket != snapshot_bucket) {
                        snapshot_bucket = bucket;
                        snapshot_due = true;
                        try {
                            save_snapshot(ensure_jpeg(), frame, detection, events.event_number());
                        } catch (const std::exception& error) {
                            Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                                     ": snapshot: ", error.what());
                        }
                    }
                }

                if (config_.timelapse_interval > 0) {
                    const auto bucket = epoch_seconds / config_.timelapse_interval;
                    if (bucket != timelapse_bucket) {
                        timelapse_bucket = bucket;
                        const auto current_hour = hour_key(frame.captured_at);
                        if (current_hour != timelapse_hour) {
                            std::string error;
                            timelapse.close(&error);
                            timelapse_hour = current_hour;
                            auto values = context(detection, frame, events.event_number());
                            const auto path = output_path(config_.timelapse_filename, ".mpg",
                                                          values, frame.captured_at);
                            std::filesystem::create_directories(path.parent_path());
                            if (!timelapse.open(path.string(), frame.width, frame.height,
                                                config_.timelapse_fps, &error)) {
                                Logger::instance().write(
                                    LogLevel::error, "camera ", config_.camera_id,
                                    ": timelapse open: ", redact_secrets(error));
                            }
                        }
                        std::string error;
                        if (timelapse.is_open() && !timelapse.write(frame, &error)) {
                            Logger::instance().write(LogLevel::warning, "camera ",
                                                     config_.camera_id,
                                                     ": timelapse write: ", redact_secrets(error));
                        }
                    }
                }

                if (decision.event_started) {
                    auto values = context(detection, frame, decision.event_number);
                    hook(config_.on_event_start, values, frame.captured_at);
                    if (config_.movie_output) {
                        movie_path = output_path(config_.movie_filename,
                                                 config_.movie_container == "mp4" ? ".mp4" : ".mkv",
                                                 values, frame.captured_at);
                        std::filesystem::create_directories(movie_path.parent_path());
                        std::string error;
                        if (movie.open(movie_path.string(), source.stream_info(), &error)) {
                            if (!movie.write_preroll(ring, &error)) {
                                Logger::instance().write(
                                    LogLevel::warning, "camera ", config_.camera_id,
                                    ": movie preroll: ", redact_secrets(error));
                            }
                            values.filename = movie_path.string();
                            values.file_type = "movie";
                            hook(config_.on_movie_start, values, frame.captured_at);
                        } else {
                            Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                                     ": movie open: ", redact_secrets(error));
                            movie_path.clear();
                        }
                    }
                }
                if (events.active() && (best_frame.captured_at.time_since_epoch().count() == 0 ||
                                        detection.changed_pixels > best_detection.changed_pixels)) {
                    best_jpeg.clear();
                    const bool draw_redbox = config_.locate_motion_mode == "preview" &&
                                             config_.locate_motion_style == "redbox" &&
                                             detection.changed_pixels > 0 &&
                                             sample.image.has_value();
                    if (config_.picture_output && draw_redbox) {
                        const auto& region = detection.region;
                        best_jpeg = source.render_jpeg(*sample.image,
                                                       RedBox{region.left, region.top, region.right,
                                                              region.bottom,
                                                              std::max(2, config_.text_scale * 2)});
                    }
                    if (config_.picture_output && best_jpeg.empty()) {
                        best_jpeg = ensure_jpeg();
                    }
                    best_frame = {frame.width, frame.height, frame.sequence, frame.captured_at, {}};
                    best_detection = detection;
                }
                if (http_ != nullptr) {
                    const auto now = std::chrono::steady_clock::now();
                    const auto period =
                        std::chrono::milliseconds(1000 / std::max(1, config_.stream_maxrate));
                    const bool stream_due = http_->wants_jpeg(std::to_string(config_.camera_id)) &&
                                            (last_http_publish.time_since_epoch().count() == 0 ||
                                             now - last_http_publish >= period);
                    if ((stream_due || snapshot_due) && !ensure_jpeg().empty()) {
                        http_->publish(std::to_string(config_.camera_id), std::move(plain_jpeg),
                                       frame.captured_at);
                        last_http_publish = now;
                    }
                }
                if (decision.event_ended) {
                    finish_event(movie, movie_path, best_jpeg, best_frame, best_detection,
                                 decision.event_number);
                    record_live = false;
                }
            }
            source.close();
            if (events.active()) {
                const auto stopped = events.stop();
                finish_event(movie, movie_path, best_jpeg, best_frame, best_detection,
                             stopped.event_number);
                record_live = false;
            }
            detector.reset();
            ring.clear();
        }

        std::string ignored;
        timelapse.close(&ignored);
        movie.close(&ignored);
        set_status([](WorkerStatus& state) {
            state.connected = false;
            state.event_active = false;
        });
    }

    CameraConfig config_;
    std::filesystem::path target_dir_;
    HookExecutor& hooks_;
    HttpServer* http_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex status_mutex_;
    WorkerStatus status_;
};

} // namespace

class Application::Impl {
  public:
    Impl(std::filesystem::path config_path, Config config)
        : config_path_(std::move(config_path)), config_(std::move(config)),
          hooks_({4, 128, std::chrono::minutes(5), std::chrono::seconds(2), "motion"}) {}

    int run() {
        apply(config_);
        while (!stopping_.load()) {
            if (reloading_.exchange(false)) {
                try {
                    auto replacement = load_config(config_path_, {true, false});
                    replacement.validate();
                    apply(std::move(replacement));
                    Logger::instance().write(LogLevel::info, "configuration reloaded");
                } catch (const std::exception& error) {
                    Logger::instance().write(LogLevel::error, "reload rejected: ", error.what());
                }
            }
            std::this_thread::sleep_for(100ms);
        }
        shutdown_workers();
        if (http_) {
            http_->stop();
        }
        hooks_.stop();
        return 0;
    }

    void request_stop() noexcept {
        stopping_.store(true);
    }
    void request_reload() noexcept {
        reloading_.store(true);
    }

  private:
    std::string status_json() const {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        std::ostringstream output;
        output << "{\"service\":\"vibe-motion\",\"cameras\":[";
        bool first = true;
        for (const auto& worker : workers_) {
            const auto state = worker->status();
            if (!first)
                output << ',';
            first = false;
            output << "{\"id\":" << worker->config().camera_id << ",\"name\":\""
                   << json_escape(worker->config().camera_name) << "\""
                   << ",\"connected\":" << (state.connected ? "true" : "false")
                   << ",\"event_active\":" << (state.event_active ? "true" : "false")
                   << ",\"event\":" << state.event_number << ",\"frames\":" << state.frames
                   << ",\"changed_pixels\":" << state.changed_pixels
                   << ",\"reconnects\":" << state.reconnects << ",\"error\":\""
                   << json_escape(redact_secrets(state.error)) << "\"}";
        }
        output << "]}";
        return output.str();
    }

    void shutdown_workers() {
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        for (auto& worker : workers_) {
            worker->request_stop();
        }
        for (auto& worker : workers_) {
            worker->join();
        }
        workers_.clear();
    }

    void apply(Config replacement) {
        shutdown_workers();
        if (http_) {
            http_->stop();
            http_.reset();
        }
        config_ = std::move(replacement);
        if (config_.global.webcontrol_port > 0) {
            HttpServerOptions options;
            options.bind_address = config_.global.webcontrol_localhost ? "127.0.0.1" : "0.0.0.0";
            options.port = static_cast<std::uint16_t>(config_.global.webcontrol_port);
            http_ = std::make_unique<HttpServer>(options, [this] { return status_json(); });
            http_->start();
        }
        for (const auto& camera : config_.cameras) {
            auto worker = std::make_unique<CameraWorker>(camera, config_.global.target_dir, hooks_,
                                                         http_.get());
            worker->start();
            {
                std::lock_guard<std::mutex> workers_lock(workers_mutex_);
                workers_.push_back(std::move(worker));
            }
        }
        Logger::instance().write(LogLevel::info, "started ", workers_.size(), " camera worker(s)");
    }

    std::filesystem::path config_path_;
    Config config_;
    HookExecutor hooks_;
    std::unique_ptr<HttpServer> http_;
    mutable std::mutex workers_mutex_;
    std::vector<std::unique_ptr<CameraWorker>> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> reloading_{false};
};

Application::Application(std::filesystem::path config_path, Config config)
    : impl_(std::make_unique<Impl>(std::move(config_path), std::move(config))) {}

Application::~Application() = default;
int Application::run() {
    return impl_->run();
}
void Application::request_stop() noexcept {
    impl_->request_stop();
}
void Application::request_reload() noexcept {
    impl_->request_reload();
}

} // namespace vibe_motion
