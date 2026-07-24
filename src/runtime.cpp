#include "vibe_motion/runtime.hpp"

#include "baichuan.hpp"
#include "runtime_util.hpp"
#include "vibe_motion/detection.hpp"
#include "vibe_motion/event.hpp"
#include "vibe_motion/hooks.hpp"
#include "vibe_motion/http.hpp"
#include "vibe_motion/log.hpp"
#include "vibe_motion/media.hpp"
#include "vibe_motion/onvif.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
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
#include <string_view>
#include <system_error>
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

std::string error_message(int error) {
    return error == 0 ? std::string{} : std::error_code(error, std::generic_category()).message();
}

void append_json_map(std::ostringstream& output, const std::map<std::string, std::string>& values) {
    output << '{';
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << '"' << json_escape(key) << "\":\"" << json_escape(value) << '"';
    }
    output << '}';
}

std::string onvif_event_json(const OnvifEvent& event) {
    std::ostringstream output;
    output << "{\"topic\":\"" << json_escape(event.topic)
           << "\",\"motion_topic\":" << (event.motion_topic ? "true" : "false")
           << ",\"has_state\":" << (event.has_state ? "true" : "false")
           << ",\"active\":" << (event.active ? "true" : "false") << ",\"utc_time\":\""
           << json_escape(event.utc_time) << "\",\"property_operation\":\""
           << json_escape(event.property_operation) << "\",\"source\":";
    append_json_map(output, event.source);
    output << ",\"key_items\":";
    append_json_map(output, event.key_items);
    output << ",\"data\":";
    append_json_map(output, event.data);
    output << ",\"raw_xml\":\"" << json_escape(event.raw_xml) << "\"}";
    return output.str();
}

std::optional<std::chrono::system_clock::time_point>
parse_onvif_utc_time(const std::string& value) {
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') {
        return std::nullopt;
    }
    std::tm parsed{};
    std::istringstream input(value.substr(0, 19));
    input >> std::get_time(&parsed, "%Y-%m-%dT%H:%M:%S");
    if (input.fail() || (value[19] != 'Z' && value[19] != '.')) {
        return std::nullopt;
    }
    auto result = std::chrono::system_clock::from_time_t(::timegm(&parsed));
    if (value[19] == 'Z') {
        return value.size() == 20 ? std::optional<std::chrono::system_clock::time_point>(result)
                                  : std::nullopt;
    }

    const auto suffix = value.find('Z', 20);
    if (suffix == std::string::npos || suffix == 20 || suffix + 1 != value.size()) {
        return std::nullopt;
    }
    std::string fractional_digits = value.substr(20, suffix - 20);
    if (!std::all_of(fractional_digits.begin(), fractional_digits.end(), [](char digit) {
            return std::isdigit(static_cast<unsigned char>(digit)) != 0;
        })) {
        return std::nullopt;
    }
    fractional_digits.resize(9, '0');
    std::int64_t nanoseconds = 0;
    for (const char digit : fractional_digits.substr(0, 9)) {
        if (!std::isdigit(static_cast<unsigned char>(digit))) {
            return std::nullopt;
        }
        nanoseconds = nanoseconds * 10 + (digit - '0');
    }
    result += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::nanoseconds(nanoseconds));
    return result;
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
    std::uint64_t effective_threshold = 0;
    std::uint8_t effective_noise_level = 0;
    std::uint64_t event_number = 0;
    std::string decode_frames = "all";
    std::string decode_requested = "all";
    std::string decode_active = "all";
    std::int64_t keyframe_interval_ms = 0;
    std::string input_transport;
    std::string input_codec;
    std::string movie_codec;
    std::string timelapse_codec;
    std::string stream_codec;
    std::string movie_error;
    std::string timelapse_error;
    std::string stream_error;
    bool onvif_events_connected = false;
    bool onvif_motion = false;
    std::uint64_t onvif_event_count = 0;
    std::string onvif_topic;
    std::string onvif_event_key;
    std::string onvif_event_utc;
    std::string onvif_event_operation;
    bool onvif_event_motion_topic = false;
    bool onvif_event_has_state = false;
    bool onvif_event_active = false;
    std::map<std::string, std::string> onvif_event_source;
    std::map<std::string, std::string> onvif_event_key_items;
    std::map<std::string, std::string> onvif_event_data;
    std::string onvif_profile;
    std::string onvif_profile_name;
    int onvif_profile_width = 0;
    int onvif_profile_height = 0;
    std::string onvif_media_error;
    std::string onvif_events_error;
    std::string error;
};

FrameDecodeMode idle_decode_mode(const CameraConfig& config) {
    if (config.decode_frames == "keyframes" ||
        (config.decode_frames == "auto" && config.events && !config.motion_detection)) {
        return FrameDecodeMode::keyframes;
    }
    return FrameDecodeMode::all;
}

bool url_unreserved(unsigned char character) {
    return std::isalnum(character) != 0 || character == '-' || character == '.' ||
           character == '_' || character == '~';
}

std::string url_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (url_unreserved(character)) {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(digits[character >> 4U]);
            result.push_back(digits[character & 0x0FU]);
        }
    }
    return result;
}

std::string url_with_userpass(std::string url, const std::string& userpass) {
    if (userpass.empty()) {
        return url;
    }
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const auto authority = scheme + 3;
    const auto authority_end = url.find_first_of("/?#", authority);
    const auto at = url.find('@', authority);
    if (at != std::string::npos && (authority_end == std::string::npos || at < authority_end)) {
        return url;
    }
    const auto colon = userpass.find(':');
    const std::string username = userpass.substr(0, colon);
    const std::string password =
        colon == std::string::npos ? std::string{} : userpass.substr(colon + 1);
    std::string scheme_name = url.substr(0, scheme);
    std::transform(
        scheme_name.begin(), scheme_name.end(), scheme_name.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::string lowered_url = url;
    std::transform(
        lowered_url.begin(), lowered_url.end(), lowered_url.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    const bool reolink_bcs_rtmp =
        scheme_name == "rtmp" && lowered_url.find("/bcs/", authority) != std::string::npos;
    const bool has_query_credentials = lowered_url.find("?user=") != std::string::npos ||
                                       lowered_url.find("&user=") != std::string::npos ||
                                       lowered_url.find("?username=") != std::string::npos ||
                                       lowered_url.find("&username=") != std::string::npos ||
                                       lowered_url.find("?password=") != std::string::npos ||
                                       lowered_url.find("&password=") != std::string::npos;
    if (reolink_bcs_rtmp && !has_query_credentials) {
        url += url.find('?') == std::string::npos ? '?' : '&';
        url += "user=" + url_encode(username) + "&password=" + url_encode(password);
        return url;
    }
    std::string credentials = url_encode(username);
    if (colon != std::string::npos) {
        credentials += ':' + url_encode(password);
    }
    url.insert(authority, credentials + '@');
    return url;
}

std::pair<std::string, std::string> split_userpass(const std::string& userpass) {
    const auto separator = userpass.find(':');
    if (separator == std::string::npos) {
        return {userpass, {}};
    }
    return {userpass.substr(0, separator), userpass.substr(separator + 1)};
}

std::string camera_url_host(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        throw std::runtime_error("camera_url has no scheme");
    }
    const auto authority_begin = scheme + 3;
    const auto authority_end = url.find_first_of("/?#", authority_begin);
    std::string authority = url.substr(authority_begin, authority_end == std::string::npos
                                                            ? std::string::npos
                                                            : authority_end - authority_begin);
    if (const auto at = authority.rfind('@'); at != std::string::npos) {
        authority.erase(0, at + 1);
    }
    if (!authority.empty() && authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string::npos || closing == 1) {
            throw std::runtime_error("camera_url has an invalid IPv6 host");
        }
        return authority.substr(1, closing - 1);
    }
    if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
        authority.resize(colon);
    }
    if (authority.empty()) {
        throw std::runtime_error("camera_url has no host");
    }
    return authority;
}

enum class SelectedMediaTransport { direct, onvif, baichuan };

const char* selected_transport_name(SelectedMediaTransport transport) {
    switch (transport) {
    case SelectedMediaTransport::direct:
        return "direct";
    case SelectedMediaTransport::onvif:
        return "onvif-rtsp";
    case SelectedMediaTransport::baichuan:
        return "baichuan";
    }
    return "unknown";
}

class CameraWorker {
  public:
    CameraWorker(CameraConfig config, std::filesystem::path target_dir, HookExecutor& hooks,
                 HttpServer* http)
        : config_(std::move(config)), target_dir_(std::move(target_dir)), hooks_(hooks),
          http_(http) {
        status_.effective_threshold = static_cast<std::uint64_t>(std::max(config_.threshold, 0));
        status_.effective_noise_level =
            static_cast<std::uint8_t>(std::clamp(config_.noise_level, 0, 255));
        status_.decode_frames = config_.decode_frames;
        const auto idle_mode = idle_decode_mode(config_);
        status_.decode_requested = std::string(frame_decode_mode_name(idle_mode));
        status_.decode_active = status_.decode_requested;
        status_.input_transport = config_.media_transport;
        status_.movie_codec = config_.movie_codec;
        status_.timelapse_codec = config_.timelapse_codec;
        status_.stream_codec = config_.stream_codec;
    }

    ~CameraWorker() {
        stop();
    }

    void start() {
        stopping_.store(false);
        if (config_.events) {
            onvif_thread_ = std::thread([this] { run_onvif_events(); });
        }
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
        if (onvif_thread_.joinable()) {
            onvif_thread_.join();
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
    BaichuanConfig baichuan_config(std::chrono::milliseconds timeout) const {
        const auto [username, password] = split_userpass(config_.camera_userpass);
        BaichuanStream stream = BaichuanStream::main;
        if (config_.media_stream == "sub") {
            stream = BaichuanStream::sub;
        } else if (config_.media_stream == "extern" || config_.media_stream == "external") {
            stream = BaichuanStream::external;
        }
        return {
            .host = camera_url_host(config_.camera_url),
            .port = static_cast<std::uint16_t>(config_.media_port),
            .username = username,
            .password = password,
            .channel = static_cast<std::uint8_t>(config_.media_channel),
            .stream = stream,
            .open_timeout = timeout,
        };
    }

    SelectedMediaTransport detect_media_transport(OnvifClient* client) {
        bool onvif_identified = false;
        std::string onvif_error;
        if (client != nullptr) {
            try {
                const OnvifDeviceInformation information = client->device_information();
                onvif_identified = true;
                Logger::instance().write(LogLevel::info, "camera ", config_.camera_id,
                                         ": ONVIF device ", information.manufacturer, " ",
                                         information.model, " (", information.firmware_version,
                                         ")");
            } catch (const std::exception& error) {
                onvif_error = error.what();
            }
        }
#if VIBE_ENABLE_BAICHUAN
        const BaichuanProbeResult probe =
            BaichuanClient::probe(baichuan_config(std::chrono::seconds(5)));
        if (probe.status == BaichuanProbeStatus::available) {
            Logger::instance().write(LogLevel::info, "camera ", config_.camera_id,
                                     ": auto media selected native Baichuan");
            return SelectedMediaTransport::baichuan;
        }
        if (probe.status == BaichuanProbeStatus::authentication_failed) {
            if (!config_.camera_userpass.empty()) {
                throw std::runtime_error("Baichuan auto-detection authentication failed: " +
                                         probe.error);
            }
            Logger::instance().write(
                LogLevel::info, "camera ", config_.camera_id,
                ": Baichuan probe had no explicit camera_userpass; preserving the configured "
                "direct URL fallback");
        } else {
            Logger::instance().write(LogLevel::info, "camera ", config_.camera_id,
                                     ": Baichuan unavailable (", redact_secrets(probe.error),
                                     "); trying the configured fallback");
        }
#else
        Logger::instance().write(LogLevel::info, "camera ", config_.camera_id,
                                 ": build has no Baichuan support");
#endif
        if (onvif_identified) {
            Logger::instance().write(LogLevel::info, "camera ", config_.camera_id,
                                     ": auto media selected ONVIF/RTSP");
            return SelectedMediaTransport::onvif;
        }
        const bool looks_like_onvif =
            runtime_detail::contains_case_insensitive(config_.camera_url, "/onvif/");
        if (looks_like_onvif) {
            throw std::runtime_error("ONVIF device identification failed: " + onvif_error);
        }
        Logger::instance().write(LogLevel::info, "camera ", config_.camera_id,
                                 ": auto media selected configured direct URL");
        return SelectedMediaTransport::direct;
    }

    OnvifClientConfig onvif_config() const {
        OnvifClientConfig result;
        result.device_url = config_.camera_url;
        result.userpass = config_.camera_userpass;
        result.profile = config_.media_profile;
        result.auth = config_.camera_auth;
        result.motion_topics = parse_onvif_topic_list(config_.events_topics);
        result.tls_verify = config_.camera_tls_verify;
        return result;
    }

    void run_onvif_events() {
        OnvifClient client(onvif_config());
        client.receive_motion_events(
            stopping_,
            [this](const OnvifEvent& event) {
                set_status([&](WorkerStatus& state) {
                    ++state.onvif_event_count;
                    state.onvif_topic = event.topic;
                    state.onvif_event_key = event.key;
                    state.onvif_event_utc = event.utc_time;
                    state.onvif_event_operation = event.property_operation;
                    state.onvif_event_motion_topic = event.motion_topic;
                    state.onvif_event_has_state = event.has_state;
                    state.onvif_event_active = event.active;
                    state.onvif_event_source = event.source;
                    state.onvif_event_key_items = event.key_items;
                    state.onvif_event_data = event.data;
                });
                if (config_.events_log) {
                    Logger::instance().write(LogLevel::info, "camera ", config_.camera_id,
                                             ": ONVIF notification ",
                                             redact_secrets(onvif_event_json(event)));
                }
                if (!event.motion_topic || !event.has_state) {
                    return;
                }
                bool active = false;
                {
                    std::lock_guard<std::mutex> lock(onvif_state_mutex_);
                    onvif_states_[event.key] = event.active;
                    if (event.active) {
                        if (const auto event_time = parse_onvif_utc_time(event.utc_time);
                            event_time && (!onvif_pending_trigger_time_ ||
                                           *event_time < *onvif_pending_trigger_time_)) {
                            onvif_pending_trigger_time_ = *event_time;
                        }
                        // Publish the generation only after its timestamp is visible.
                        onvif_trigger_generation_.fetch_add(1);
                    }
                    active = std::any_of(onvif_states_.begin(), onvif_states_.end(),
                                         [](const auto& item) { return item.second; });
                }
                onvif_motion_.store(active);
                set_status([&](WorkerStatus& state) { state.onvif_motion = active; });
            },
            [this](bool connected, const std::string& error) {
                if (!connected) {
                    {
                        std::lock_guard<std::mutex> lock(onvif_state_mutex_);
                        onvif_states_.clear();
                        onvif_pending_trigger_time_.reset();
                    }
                    onvif_motion_.store(false);
                }
                set_status([&](WorkerStatus& state) {
                    state.onvif_events_connected = connected;
                    state.onvif_motion = connected && onvif_motion_.load();
                    state.onvif_events_error = redact_secrets(error);
                });
                if (!error.empty()) {
                    Logger::instance().write(LogLevel::warning, "camera ", config_.camera_id,
                                             ": ONVIF events: ", redact_secrets(error));
                }
            });
    }

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
        result.noise_level = detection.effective_noise_level;
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
              std::chrono::system_clock::time_point when, const std::string& kind,
              HookPriority priority = HookPriority::normal, std::string coalesce_key = {}) {
        if (command.empty()) {
            return;
        }
        try {
            auto argv = parse_hook_command(command);
            for (auto& argument : argv) {
                argument = expand_template(argument, values, when);
            }
            if (!hooks_.submit(std::move(argv),
                               {.priority = priority,
                                .kind = kind,
                                .camera_id = config_.camera_id,
                                .serial_key = "camera:" + std::to_string(config_.camera_id),
                                .coalesce_key = std::move(coalesce_key)})) {
                const auto status = hooks_.status();
                Logger::instance().write(
                    LogLevel::warning, "camera ", config_.camera_id, ": hook dropped kind=", kind,
                    " pending=", status.pending, '/', status.max_pending,
                    " running=", status.running, '/', status.max_concurrent,
                    " supervisor=", status.supervisor_healthy ? "healthy" : "failed");
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
        hook(config_.on_picture_save, values, frame.captured_at, "snapshot", HookPriority::normal,
             "snapshot:" + std::to_string(config_.camera_id));
    }

    void finish_event(EventMovieWriter& movie, std::filesystem::path& movie_path,
                      std::vector<std::uint8_t>& best_jpeg, GrayFrame& best_frame,
                      DetectionResult& best_detection, std::uint64_t event_number) {
        const auto picture_when = best_frame.captured_at.time_since_epoch().count() == 0
                                      ? std::chrono::system_clock::now()
                                      : best_frame.captured_at;
        const auto end_when = std::chrono::system_clock::now();
        auto values = context(best_detection, best_frame, event_number);
        std::optional<ExpansionContext> picture_values;
        if (config_.picture_output && !best_jpeg.empty()) {
            const auto picture =
                output_path(config_.picture_filename, ".jpg", values, picture_when);
            try {
                write_atomic(picture, best_jpeg);
                picture_values = values;
                picture_values->filename = picture.string();
                picture_values->file_type = "picture";
            } catch (const std::exception& error) {
                Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                         ": picture write failed: ", error.what());
            }
        }
        std::optional<ExpansionContext> movie_values;
        if (movie.is_open()) {
            std::string error;
            movie.close(&error);
            movie_values = values;
            movie_values->filename = movie_path.string();
            movie_values->file_type = "movie";
            if (!error.empty()) {
                set_status([&](WorkerStatus& state) { state.movie_error = redact_secrets(error); });
                Logger::instance().write(LogLevel::warning, "camera ", config_.camera_id,
                                         ": movie finalize: ", redact_secrets(error));
            }
        }
        values.filename.clear();
        values.file_type.clear();
        hook(config_.on_event_end, values, end_when, "event-end", HookPriority::critical);
        if (picture_values) {
            hook(config_.on_picture_save, *picture_values, picture_when, "event-picture",
                 HookPriority::critical);
        }
        if (movie_values) {
            hook(config_.on_movie_end, *movie_values, end_when, "movie-end",
                 HookPriority::critical);
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
        detection_settings.noise_tune = config_.noise_tune;
        detection_settings.lightswitch_percent = config_.lightswitch_percent;
        detection_settings.despeckle = !config_.despeckle_filter.empty();
        MotionDetector detector(detection_settings);
        int loaded_mask_width = 0;
        int loaded_mask_height = 0;
        const auto load_mask = [&](int width, int height) {
            if (width <= 0 || height <= 0 ||
                (width == loaded_mask_width && height == loaded_mask_height)) {
                return true;
            }
            try {
                detector.load_pgm_mask(config_.mask_file, width, height);
                loaded_mask_width = width;
                loaded_mask_height = height;
                return true;
            } catch (const std::exception& error) {
                Logger::instance().write(LogLevel::error, "camera ", config_.camera_id, ": ",
                                         error.what());
                set_status([&](WorkerStatus& state) { state.error = error.what(); });
                return false;
            }
        };

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
        std::uint64_t seen_onvif_trigger = 0;
        int onvif_trigger_frames = 0;
        std::optional<std::chrono::system_clock::time_point> pending_onvif_event_time;
        std::optional<OnvifStream> onvif_stream;
        std::optional<OnvifClient> onvif_client;
        if (runtime_detail::http_camera_url(config_.camera_url) &&
            (config_.media_transport == "auto" || config_.media_transport == "onvif")) {
            onvif_client.emplace(onvif_config());
        }
        std::optional<SelectedMediaTransport> selected_transport;
        if (config_.media_transport == "direct") {
            selected_transport = SelectedMediaTransport::direct;
        } else if (config_.media_transport == "onvif") {
            selected_transport = SelectedMediaTransport::onvif;
        } else if (config_.media_transport == "baichuan") {
            selected_transport = SelectedMediaTransport::baichuan;
        }
        const auto configured_idle_decode_mode = idle_decode_mode(config_);
        const bool dynamic_full_decode = config_.decode_frames == "auto" &&
                                         configured_idle_decode_mode == FrameDecodeMode::keyframes;
        const std::string camera_key = std::to_string(config_.camera_id);
        timelapse.set_packet_callback([this, camera_key](const VideoPacket& packet) {
            if (http_ != nullptr)
                http_->publish_timelapse_video(camera_key, packet);
        });

        while (!stopping_.load()) {
            if (!selected_transport.has_value()) {
                try {
                    selected_transport =
                        detect_media_transport(onvif_client.has_value() ? &*onvif_client : nullptr);
                } catch (const std::exception& error) {
                    const std::string safe_error = redact_secrets(error.what());
                    set_status([&](WorkerStatus& state) {
                        state.connected = false;
                        ++state.reconnects;
                        state.error = safe_error;
                        state.onvif_media_error = safe_error;
                    });
                    Logger::instance().write(LogLevel::warning, "camera ", config_.camera_id,
                                             ": auto media detection: ", safe_error);
                    for (int tenth = 0; tenth < 10 && !stopping_.load(); ++tenth) {
                        std::this_thread::sleep_for(100ms);
                    }
                    continue;
                }
            }
            const bool use_baichuan = *selected_transport == SelectedMediaTransport::baichuan;
            const bool use_onvif = *selected_transport == SelectedMediaTransport::onvif;
            std::string media_url = config_.camera_url;
            int analysis_width = config_.width;
            int analysis_height = config_.height;
            if (use_baichuan) {
                if (!config_.width_configured) {
                    analysis_width = 0;
                }
                if (!config_.height_configured) {
                    analysis_height = 0;
                }
            } else if (use_onvif) {
                try {
                    if (!onvif_client.has_value()) {
                        throw std::runtime_error("ONVIF media client is unavailable");
                    }
                    OnvifStream resolved = onvif_client->resolve_stream();
                    const bool newly_resolved =
                        !onvif_stream || onvif_stream->uri != resolved.uri ||
                        onvif_stream->profile_token != resolved.profile_token;
                    onvif_stream = std::move(resolved);
                    const OnvifStream& stream = *onvif_stream;
                    media_url = stream.uri;
                    if (!config_.width_configured) {
                        analysis_width = stream.width;
                    }
                    if (!config_.height_configured) {
                        analysis_height = stream.height;
                    }
                    set_status([&](WorkerStatus& state) {
                        state.onvif_profile = stream.profile_token;
                        state.onvif_profile_name = stream.profile_name;
                        state.onvif_profile_width = stream.width;
                        state.onvif_profile_height = stream.height;
                        state.onvif_media_error.clear();
                    });
                    if (newly_resolved) {
                        Logger::instance().write(LogLevel::info, "camera ", config_.camera_id,
                                                 ": ONVIF profile ", stream.profile_name, " (",
                                                 stream.profile_token, ", ", stream.width, "x",
                                                 stream.height, ") resolved media stream");
                    }
                } catch (const std::exception& error) {
                    const std::string safe_error = redact_secrets(error.what());
                    set_status([&](WorkerStatus& state) {
                        state.connected = false;
                        ++state.reconnects;
                        state.error = safe_error;
                        state.onvif_media_error = safe_error;
                    });
                    Logger::instance().write(LogLevel::warning, "camera ", config_.camera_id,
                                             ": ONVIF media discovery: ", safe_error);
                    for (int tenth = 0; tenth < 10 && !stopping_.load(); ++tenth) {
                        std::this_thread::sleep_for(100ms);
                    }
                    continue;
                }
            }
            CameraSourceConfig source_config;
            if (use_baichuan) {
                const auto [username, password] = split_userpass(config_.camera_userpass);
                source_config.baichuan = BaichuanSourceConfig{
                    .host = camera_url_host(config_.camera_url),
                    .port = config_.media_port,
                    .username = username,
                    .password = password,
                    .channel = config_.media_channel,
                    .stream = config_.media_stream,
                };
            } else {
                source_config.url =
                    url_with_userpass(std::move(media_url), config_.camera_userpass);
            }
            source_config.width = analysis_width;
            source_config.height = analysis_height;
            source_config.analysis_framerate = config_.framerate;
            source_config.jpeg_quality = config_.movie_quality;
            source_config.decode_mode = configured_idle_decode_mode;
            const auto net_options = parse_netcam_options(config_.media_options);
            if (const auto found = net_options.find("interrupt"); found != net_options.end()) {
                try {
                    source_config.io_timeout = std::chrono::seconds(std::stoi(found->second));
                } catch (...) { /* config validation owns diagnostics */
                }
            }
            if (config_.media_use_tcp || (net_options.count("rtsp_transport") &&
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
            FrameDecodeController decode_controller(configured_idle_decode_mode);
            auto reported_requested_mode = decode_controller.requested_mode();
            auto reported_active_mode = decode_controller.active_mode();
            std::optional<std::chrono::steady_clock::time_point> last_keyframe_at;
            const std::string input_codec = source.stream_info().codec_name();
            const std::string normalized_stream_codec = normalize_video_codec(config_.stream_codec);
            const bool stream_passthrough_supported =
                normalized_stream_codec != "copy" || input_codec == "h264" || input_codec == "hevc";
            const bool publish_video_packets =
                http_ != nullptr && config_.stream_codec != "mjpeg" && stream_passthrough_supported;
            set_status([&](WorkerStatus& state) {
                state.connected = true;
                state.error.clear();
                state.input_transport = selected_transport_name(*selected_transport);
                state.input_codec = input_codec;
                state.movie_codec = normalize_video_codec(config_.movie_codec) == "copy"
                                        ? input_codec
                                        : normalize_video_codec(config_.movie_codec);
                state.timelapse_codec = normalize_video_codec(config_.timelapse_codec);
                state.stream_codec =
                    config_.stream_codec == "mjpeg"
                        ? "mjpeg"
                        : (normalized_stream_codec == "copy" ? input_codec
                                                             : normalized_stream_codec);
                state.movie_error.clear();
                state.timelapse_error.clear();
                state.stream_error.clear();
                if (!stream_passthrough_supported) {
                    state.stream_error =
                        "fragmented MP4 passthrough requires H.264 or HEVC camera input";
                }
                state.decode_requested =
                    std::string(frame_decode_mode_name(decode_controller.requested_mode()));
                state.decode_active =
                    std::string(frame_decode_mode_name(decode_controller.active_mode()));
                state.keyframe_interval_ms = 0;
            });
            if (config_.motion_detection && !config_.mask_file.empty()) {
                const int mask_width =
                    analysis_width > 0 ? analysis_width : source.stream_info().width();
                const int mask_height =
                    analysis_height > 0 ? analysis_height : source.stream_info().height();
                if (mask_width <= 0 || mask_height <= 0) {
                    Logger::instance().write(
                        LogLevel::info, "camera ", config_.camera_id,
                        ": deferring mask load until the first decoded frame provides dimensions");
                } else if (!load_mask(mask_width, mask_height)) {
                    source.close();
                    return;
                }
            }
            Logger::instance().write(LogLevel::info, "camera ", config_.camera_id, " connected (",
                                     source.stream_info().codec_name(), ")");
            const auto warm_until = std::chrono::steady_clock::now() + 2s;
            const auto update_decode_mode = [&](bool keyframe_boundary) {
                const auto now = std::chrono::steady_clock::now();
                const bool wants_full_decode =
                    dynamic_full_decode && http_ != nullptr && http_->wants_jpeg(camera_key);
                if (const auto transition =
                        decode_controller.update(wants_full_decode, keyframe_boundary, now)) {
                    source.set_decode_mode(*transition);
                }
                if (decode_controller.requested_mode() != reported_requested_mode ||
                    decode_controller.active_mode() != reported_active_mode) {
                    reported_requested_mode = decode_controller.requested_mode();
                    reported_active_mode = decode_controller.active_mode();
                    set_status([&](WorkerStatus& state) {
                        state.decode_requested =
                            std::string(frame_decode_mode_name(reported_requested_mode));
                        state.decode_active =
                            std::string(frame_decode_mode_name(reported_active_mode));
                    });
                }
            };

            while (!stopping_.load()) {
                update_decode_mode(false);
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
                if (publish_video_packets && sample.packet.valid()) {
                    const VideoEncodeOptions stream_options{
                        .quality = config_.stream_quality,
                        .bitrate = config_.stream_bitrate,
                        .codec = config_.stream_codec,
                        .encoder = config_.stream_encoder,
                        .keyframe_interval = config_.stream_keyframe_interval,
                        .low_latency = true,
                    };
                    http_->publish_video(camera_key, sample.packet, stream_options);
                }
                if (sample.decoded_keyframe) {
                    const auto now = std::chrono::steady_clock::now();
                    if (last_keyframe_at) {
                        const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - *last_keyframe_at);
                        set_status([&](WorkerStatus& state) {
                            state.keyframe_interval_ms = interval.count();
                        });
                    }
                    last_keyframe_at = now;
                }
                update_decode_mode(sample.packet.keyframe() || sample.decoded_keyframe);
                if (!sample.frame) {
                    if (movie.is_open() && record_live) {
                        std::string error;
                        if (!movie.write(sample.packet, &error)) {
                            set_status([&](WorkerStatus& state) {
                                state.movie_error = redact_secrets(error);
                            });
                            Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                                     ": movie write: ", redact_secrets(error));
                        }
                    }
                    continue;
                }
                auto& frame = *sample.frame;
                DetectionResult detection;
                if (config_.motion_detection) {
                    if (!config_.mask_file.empty() && !load_mask(frame.width, frame.height)) {
                        source.close();
                        return;
                    }
                    detection = detector.process(frame);
                } else {
                    detection.effective_threshold =
                        static_cast<std::uint64_t>(std::max(config_.threshold, 0));
                    detection.effective_noise_level =
                        static_cast<std::uint8_t>(std::clamp(config_.noise_level, 0, 255));
                }
                const bool warmed = std::chrono::steady_clock::now() >= warm_until;
                const std::uint64_t onvif_generation = onvif_trigger_generation_.load();
                bool onvif_edge = false;
                if (onvif_generation != seen_onvif_trigger) {
                    seen_onvif_trigger = onvif_generation;
                    onvif_trigger_frames = 1;
                    onvif_edge = true;
                    std::optional<std::chrono::system_clock::time_point> trigger_time;
                    {
                        std::lock_guard<std::mutex> lock(onvif_state_mutex_);
                        trigger_time = onvif_pending_trigger_time_;
                        onvif_pending_trigger_time_.reset();
                    }
                    if (!events.active()) {
                        pending_onvif_event_time = trigger_time;
                    }
                }
                const bool onvif_triggered = onvif_motion_.load() || onvif_trigger_frames > 0;
                const bool qualifying_motion =
                    (config_.motion_detection && warmed && detection.motion) ||
                    (config_.events && onvif_triggered);
                const auto decision =
                    events.update(qualifying_motion, std::chrono::steady_clock::now(),
                                  config_.events && onvif_edge);
                if (onvif_trigger_frames > 0) {
                    --onvif_trigger_frames;
                }
                const bool record_frame =
                    decision.record_frame || (config_.movie_all_frames && events.active());
                if (movie.is_open() && !decision.event_started && record_frame) {
                    std::string error;
                    if (!movie.write(sample.packet, &error)) {
                        set_status([&](WorkerStatus& state) {
                            state.movie_error = redact_secrets(error);
                        });
                        Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                                 ": movie write: ", redact_secrets(error));
                    }
                }
                record_live = record_frame;
                set_status([&](WorkerStatus& state) {
                    state.frames++;
                    state.changed_pixels = detection.changed_pixels;
                    state.effective_threshold = detection.effective_threshold;
                    state.effective_noise_level = detection.effective_noise_level;
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

                // Motion runs event actions before the periodic snapshot action.
                // This ordering lets a snapshot on the trigger frame attach to
                // the event that has just been announced to the hook scripts.
                if (decision.event_started) {
                    const auto event_when = pending_onvif_event_time.value_or(frame.captured_at);
                    pending_onvif_event_time.reset();
                    auto values = context(detection, frame, decision.event_number);
                    hook(config_.on_event_start, values, event_when, "event-start",
                         HookPriority::critical);
                    if (config_.movie_output) {
                        movie_path = output_path(config_.movie_filename,
                                                 config_.movie_container == "mp4" ? ".mp4" : ".mkv",
                                                 values, event_when);
                        std::filesystem::create_directories(movie_path.parent_path());
                        std::string error;
                        const VideoEncodeOptions movie_options{
                            .quality = config_.movie_quality,
                            .bitrate = config_.movie_bitrate,
                            .codec = config_.movie_codec,
                            .encoder = config_.movie_encoder,
                            .keyframe_interval = config_.movie_keyframe_interval,
                        };
                        if (movie.open(movie_path.string(), source.stream_info(), movie_options,
                                       &error)) {
                            set_status([](WorkerStatus& state) { state.movie_error.clear(); });
                            values.filename = movie_path.string();
                            values.file_type = "movie";
                            if (!movie.write_preroll(ring, &error)) {
                                Logger::instance().write(
                                    LogLevel::warning, "camera ", config_.camera_id,
                                    ": movie preroll: ", redact_secrets(error));
                            }
                            hook(config_.on_movie_start, values, event_when, "movie-start",
                                 HookPriority::critical);
                        } else {
                            set_status([&](WorkerStatus& state) {
                                state.movie_error = redact_secrets(error);
                            });
                            Logger::instance().write(LogLevel::error, "camera ", config_.camera_id,
                                                     ": movie open: ", redact_secrets(error));
                            std::error_code ignored;
                            std::filesystem::remove(movie_path, ignored);
                            movie_path.clear();
                        }
                    }
                }
                if (decision.event_ended) {
                    finish_event(movie, movie_path, best_jpeg, best_frame, best_detection,
                                 decision.event_number);
                    record_live = false;
                }

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
                            const auto path =
                                output_path(config_.timelapse_filename,
                                            timelapse_file_extension(config_.timelapse_container),
                                            values, frame.captured_at);
                            std::filesystem::create_directories(path.parent_path());
                            const TimelapseEncodeOptions encode_options{
                                .quality = config_.timelapse_quality,
                                .bitrate = config_.timelapse_bitrate,
                                .codec = config_.timelapse_codec,
                                .encoder = config_.timelapse_encoder,
                                .keyframe_interval = config_.timelapse_keyframe_interval,
                            };
                            if (!timelapse.open(path.string(), frame.width, frame.height,
                                                config_.timelapse_fps, encode_options, &error)) {
                                set_status([&](WorkerStatus& state) {
                                    state.timelapse_error = redact_secrets(error);
                                });
                                Logger::instance().write(
                                    LogLevel::error, "camera ", config_.camera_id,
                                    ": timelapse open: ", redact_secrets(error));
                            } else {
                                set_status(
                                    [](WorkerStatus& state) { state.timelapse_error.clear(); });
                            }
                        }
                        std::string error;
                        if (timelapse.is_open()) {
                            if (!sample.image) {
                                error = "decoded color frame is unavailable";
                            } else if (timelapse.write(*sample.image, &error)) {
                                error.clear();
                            }
                            if (!error.empty()) {
                                Logger::instance().write(
                                    LogLevel::warning, "camera ", config_.camera_id,
                                    ": timelapse write: ", redact_secrets(error));
                            }
                        }
                    }
                }

                const bool missing_event_picture =
                    best_frame.captured_at.time_since_epoch().count() == 0;
                const bool better_motion_picture =
                    config_.motion_detection &&
                    detection.changed_pixels > best_detection.changed_pixels;
                if (events.active() && (missing_event_picture || better_motion_picture)) {
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
                    const bool stream_due = http_->wants_jpeg(camera_key) &&
                                            (last_http_publish.time_since_epoch().count() == 0 ||
                                             now - last_http_publish >= period);
                    if ((stream_due || snapshot_due) && !ensure_jpeg().empty()) {
                        http_->publish(std::to_string(config_.camera_id), std::move(plain_jpeg),
                                       frame.captured_at);
                        last_http_publish = now;
                    }
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
    std::thread onvif_thread_;
    std::atomic<bool> onvif_motion_{false};
    std::atomic<std::uint64_t> onvif_trigger_generation_{0};
    std::mutex onvif_state_mutex_;
    std::unordered_map<std::string, bool> onvif_states_;
    std::optional<std::chrono::system_clock::time_point> onvif_pending_trigger_time_;
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
        const auto hook_status = hooks_.status();
        std::lock_guard<std::mutex> workers_lock(workers_mutex_);
        std::ostringstream output;
        output << "{\"service\":\"vibe-motion\",\"hooks\":{\"pending\":" << hook_status.pending
               << ",\"running\":" << hook_status.running
               << ",\"max_pending\":" << hook_status.max_pending
               << ",\"max_concurrent\":" << hook_status.max_concurrent
               << ",\"submitted\":" << hook_status.submitted
               << ",\"completed\":" << hook_status.completed
               << ",\"timed_out\":" << hook_status.timed_out << ",\"failed\":" << hook_status.failed
               << ",\"dropped\":" << hook_status.dropped
               << ",\"coalesced\":" << hook_status.coalesced
               << ",\"backpressure\":" << hook_status.backpressure
               << ",\"supervisor_healthy\":" << (hook_status.supervisor_healthy ? "true" : "false")
               << ",\"supervisor_restarts\":" << hook_status.supervisor_restarts
               << ",\"last_error\":" << hook_status.last_error << ",\"last_error_text\":\""
               << json_escape(error_message(hook_status.last_error)) << "\"},\"cameras\":[";
        bool first = true;
        for (const auto& worker : workers_) {
            const auto state = worker->status();
            std::string onvif_error = state.onvif_media_error;
            if (!state.onvif_events_error.empty()) {
                if (!onvif_error.empty()) {
                    onvif_error += "; ";
                }
                onvif_error += state.onvif_events_error;
            }
            if (!first)
                output << ',';
            first = false;
            output << "{\"id\":" << worker->config().camera_id << ",\"name\":\""
                   << json_escape(worker->config().camera_name) << "\""
                   << ",\"connected\":" << (state.connected ? "true" : "false")
                   << ",\"event_active\":" << (state.event_active ? "true" : "false")
                   << ",\"event\":" << state.event_number << ",\"frames\":" << state.frames
                   << ",\"changed_pixels\":" << state.changed_pixels
                   << ",\"threshold\":" << state.effective_threshold
                   << ",\"noise_level\":" << static_cast<unsigned>(state.effective_noise_level)
                   << ",\"reconnects\":" << state.reconnects << ",\"error\":\""
                   << json_escape(redact_secrets(state.error)) << "\""
                   << ",\"decode_frames\":\"" << json_escape(state.decode_frames) << "\""
                   << ",\"decode_requested\":\"" << json_escape(state.decode_requested) << "\""
                   << ",\"decode_active\":\"" << json_escape(state.decode_active) << "\""
                   << ",\"keyframe_interval_ms\":" << state.keyframe_interval_ms
                   << ",\"input_transport\":\"" << json_escape(state.input_transport) << "\""
                   << ",\"input_codec\":\"" << json_escape(state.input_codec) << "\""
                   << ",\"movie_codec\":\"" << json_escape(state.movie_codec) << "\""
                   << ",\"timelapse_codec\":\"" << json_escape(state.timelapse_codec) << "\""
                   << ",\"stream_codec\":\"" << json_escape(state.stream_codec) << "\""
                   << ",\"movie_error\":\"" << json_escape(state.movie_error) << "\""
                   << ",\"timelapse_error\":\"" << json_escape(state.timelapse_error) << "\""
                   << ",\"stream_error\":\"" << json_escape(state.stream_error) << "\""
                   << ",\"onvif_profile\":\"" << json_escape(state.onvif_profile) << "\""
                   << ",\"onvif_profile_name\":\"" << json_escape(state.onvif_profile_name)
                   << "\",\"onvif_profile_width\":" << state.onvif_profile_width
                   << ",\"onvif_profile_height\":" << state.onvif_profile_height
                   << ",\"onvif_events_connected\":"
                   << (state.onvif_events_connected ? "true" : "false")
                   << ",\"onvif_motion\":" << (state.onvif_motion ? "true" : "false")
                   << ",\"onvif_event_count\":" << state.onvif_event_count << ",\"onvif_topic\":\""
                   << json_escape(state.onvif_topic) << "\""
                   << ",\"onvif_last_event\":{\"key\":\"" << json_escape(state.onvif_event_key)
                   << "\",\"utc_time\":\"" << json_escape(state.onvif_event_utc)
                   << "\",\"property_operation\":\"" << json_escape(state.onvif_event_operation)
                   << "\",\"motion_topic\":" << (state.onvif_event_motion_topic ? "true" : "false")
                   << ",\"has_state\":" << (state.onvif_event_has_state ? "true" : "false")
                   << ",\"active\":" << (state.onvif_event_active ? "true" : "false")
                   << ",\"source\":";
            append_json_map(output, state.onvif_event_source);
            output << ",\"key_items\":";
            append_json_map(output, state.onvif_event_key_items);
            output << ",\"data\":";
            append_json_map(output, state.onvif_event_data);
            output << '}' << ",\"onvif_media_error\":\""
                   << json_escape(redact_secrets(state.onvif_media_error))
                   << "\",\"onvif_events_error\":\""
                   << json_escape(redact_secrets(state.onvif_events_error))
                   << "\",\"onvif_error\":\"" << json_escape(redact_secrets(onvif_error)) << "\"}";
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
