#include "baichuan.hpp"

#include "baichuan_protocol.hpp"

extern "C" {
#include <libavutil/aes.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace vibe_motion {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr std::uint32_t wire_magic = 0x0abcdef0;
constexpr std::uint16_t modern_class = 0x6414;
constexpr std::uint16_t login_upgrade_class = 0x6514;
constexpr std::uint32_t login_message = 1;
constexpr std::uint32_t video_message = 3;
constexpr std::uint32_t video_stop_message = 4;
constexpr std::uint32_t keepalive_message = 234;
constexpr std::size_t maximum_wire_body = std::size_t{32} * 1024U * 1024U;
constexpr std::size_t maximum_media_buffer = std::size_t{64} * 1024U * 1024U;
constexpr std::size_t initial_frame_count = 9;

void set_error(std::string* destination, const std::string& value) {
    if (destination != nullptr) {
        *destination = value;
    }
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
}

int remaining_ms(Deadline deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) {
        return 0;
    }
    return static_cast<int>(std::min<std::int64_t>(remaining, std::numeric_limits<int>::max()));
}

std::string socket_error(std::string_view operation, int value = errno) {
    return std::string(operation) + ": " + std::strerror(value);
}

bool wait_socket(int socket, short events, Deadline deadline, std::string* error) {
    for (;;) {
        const int wait = remaining_ms(deadline);
        if (wait == 0) {
            return false;
        }
        pollfd descriptor{socket, events, 0};
        const int result = ::poll(&descriptor, 1, wait);
        if (result > 0) {
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                int socket_status = 0;
                socklen_t size = sizeof(socket_status);
                if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &socket_status, &size) == 0 &&
                    socket_status != 0) {
                    set_error(error, socket_error("Baichuan socket failed", socket_status));
                } else {
                    set_error(error, "Baichuan camera closed the connection");
                }
                return false;
            }
            if ((descriptor.revents & events) != 0) {
                return true;
            }
            continue;
        }
        if (result == 0) {
            return false;
        }
        if (errno != EINTR) {
            set_error(error, socket_error("cannot poll Baichuan socket"));
            return false;
        }
    }
}

bool send_all(int socket, std::span<const std::uint8_t> bytes, Deadline deadline,
              std::string* error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!wait_socket(socket, POLLOUT, deadline, error)) {
            if (error != nullptr && error->empty()) {
                *error = "Baichuan send timed out";
            }
            return false;
        }
        const ssize_t written =
            ::send(socket, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        set_error(error, written == 0 ? "Baichuan camera closed while sending"
                                      : socket_error("cannot send to Baichuan camera"));
        return false;
    }
    return true;
}

int connect_tcp(const BaichuanConfig& config, Deadline deadline, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(config.port);
    const int resolved = ::getaddrinfo(config.host.c_str(), service.c_str(), &hints, &addresses);
    if (resolved != 0) {
        set_error(error, "cannot resolve Baichuan host: " + std::string(gai_strerror(resolved)));
        return -1;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> holder(addresses, &freeaddrinfo);
    std::string last_error = "cannot connect to Baichuan camera";
    for (const addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int socket =
            ::socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC, address->ai_protocol);
        if (socket < 0) {
            last_error = socket_error("cannot create Baichuan socket");
            continue;
        }
        const int flags = ::fcntl(socket, F_GETFL, 0);
        if (flags < 0 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error = socket_error("cannot configure Baichuan socket");
            ::close(socket);
            continue;
        }
        int connected = ::connect(socket, address->ai_addr, address->ai_addrlen);
        if (connected < 0 && errno == EINPROGRESS) {
            std::string wait_error;
            if (wait_socket(socket, POLLOUT, deadline, &wait_error)) {
                int status = 0;
                socklen_t size = sizeof(status);
                if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &status, &size) == 0 &&
                    status == 0) {
                    connected = 0;
                } else {
                    last_error = socket_error("cannot connect to Baichuan camera",
                                              status != 0 ? status : errno);
                }
            } else if (!wait_error.empty()) {
                last_error = std::move(wait_error);
            } else {
                last_error = "Baichuan connection timed out";
            }
        } else if (connected < 0) {
            last_error = socket_error("cannot connect to Baichuan camera");
        }
        if (connected == 0) {
            return socket;
        }
        ::close(socket);
        if (remaining_ms(deadline) == 0) {
            break;
        }
    }
    set_error(error, last_error);
    return -1;
}

std::string xml_tag(std::string_view xml, std::string_view tag) {
    const std::string opening = "<" + std::string(tag) + ">";
    const std::string closing = "</" + std::string(tag) + ">";
    const std::size_t begin = xml.find(opening);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t value_begin = begin + opening.size();
    const std::size_t end = xml.find(closing, value_begin);
    if (end == std::string_view::npos) {
        return {};
    }
    return std::string(xml.substr(value_begin, end - value_begin));
}

std::optional<std::uint32_t> xml_unsigned(std::string_view xml, std::string_view tag) {
    const std::string text = xml_tag(xml, tag);
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<unsigned>(character - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

std::string xml_document(std::string_view body) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" ?><body>" + std::string(body) + "</body>";
}

enum class Encryption { none, bc_xor, aes, full_aes };

struct WirePacket {
    baichuan_detail::WireHeader header;
    std::vector<std::uint8_t> body;
};

struct ParsedPayload {
    bool binary = false;
    std::vector<std::uint8_t> bytes;
};

bool is_media_magic(std::uint32_t magic) {
    return magic == 0x31303031 || magic == 0x32303031 ||
           (magic >= 0x63643030 && magic <= 0x63643039) ||
           (magic >= 0x63643130 && magic <= 0x63643139) || magic == 0x62773530 ||
           magic == 0x62773130;
}

struct MediaRecord {
    enum class Type { information, video, ignored };
    Type type = Type::ignored;
    BaichuanCodec codec = BaichuanCodec::h264;
    int width = 0;
    int height = 0;
    int reported_fps = 0;
    BaichuanFrame frame;
};

class MediaParser {
  public:
    void append(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > maximum_media_buffer ||
            buffer_.size() > maximum_media_buffer - bytes.size()) {
            throw std::runtime_error("Baichuan media buffer exceeded safety limit");
        }
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    std::optional<MediaRecord> next() {
        for (;;) {
            if (buffer_.size() - offset_ < 4) {
                compact();
                return std::nullopt;
            }
            const std::span<const std::uint8_t> remaining(buffer_.data() + offset_,
                                                          buffer_.size() - offset_);
            const std::uint32_t magic = read_u32(remaining, 0);
            if (!is_media_magic(magic)) {
                ++offset_;
                continue;
            }
            if (magic == 0x31303031 || magic == 0x32303031) {
                if (remaining.size() < 36) {
                    compact();
                    return std::nullopt;
                }
                if (read_u32(remaining, 4) != 32) {
                    ++offset_;
                    continue;
                }
                MediaRecord result;
                result.type = MediaRecord::Type::information;
                result.width = static_cast<int>(read_u32(remaining, 8));
                result.height = static_cast<int>(read_u32(remaining, 12));
                result.reported_fps = static_cast<int>(remaining[17]);
                offset_ += 36;
                compact();
                return result;
            }
            if ((magic >= 0x63643030 && magic <= 0x63643039) ||
                (magic >= 0x63643130 && magic <= 0x63643139)) {
                if (remaining.size() < 24) {
                    compact();
                    return std::nullopt;
                }
                const bool h264 = std::equal(remaining.begin() + 4, remaining.begin() + 8,
                                             std::string_view("H264").begin());
                const bool h265 = std::equal(remaining.begin() + 4, remaining.begin() + 8,
                                             std::string_view("H265").begin());
                if (!h264 && !h265) {
                    ++offset_;
                    continue;
                }
                const std::uint32_t payload_size = read_u32(remaining, 8);
                const std::uint32_t additional_size = read_u32(remaining, 12);
                if (payload_size > maximum_wire_body || additional_size > 4096) {
                    throw std::runtime_error("invalid Baichuan video frame size");
                }
                const std::size_t padding = (8U - (payload_size % 8U)) % 8U;
                const std::uint64_t total64 = 24ULL + additional_size + payload_size + padding;
                if (total64 > maximum_media_buffer) {
                    throw std::runtime_error("invalid Baichuan video record length");
                }
                const std::size_t total = static_cast<std::size_t>(total64);
                if (remaining.size() < total) {
                    compact();
                    return std::nullopt;
                }
                const std::size_t payload_begin = 24U + additional_size;
                MediaRecord result;
                result.type = MediaRecord::Type::video;
                result.codec = h265 ? BaichuanCodec::hevc : BaichuanCodec::h264;
                result.frame.timestamp_us = read_u32(remaining, 16);
                result.frame.keyframe = magic >= 0x63643030 && magic <= 0x63643039;
                result.frame.data.assign(
                    remaining.begin() + static_cast<std::ptrdiff_t>(payload_begin),
                    remaining.begin() + static_cast<std::ptrdiff_t>(payload_begin + payload_size));
                offset_ += total;
                compact();
                return result;
            }
            if (remaining.size() < 8) {
                compact();
                return std::nullopt;
            }
            const std::uint16_t payload_size = read_u16(remaining, 4);
            const std::size_t padding = (8U - (payload_size % 8U)) % 8U;
            constexpr std::size_t header_size = 8;
            const std::uint64_t total64 =
                static_cast<std::uint64_t>(header_size) + payload_size + padding;
            if (total64 > maximum_media_buffer) {
                throw std::runtime_error("invalid Baichuan audio record length");
            }
            const std::size_t total = static_cast<std::size_t>(total64);
            if (remaining.size() < total) {
                compact();
                return std::nullopt;
            }
            offset_ += total;
            compact();
        }
    }

  private:
    void compact() {
        if (offset_ == 0) {
            return;
        }
        if (offset_ == buffer_.size() || offset_ >= std::size_t{1024} * 1024U) {
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
            offset_ = 0;
        }
    }

    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
};

} // namespace

namespace baichuan_detail {

std::vector<std::uint8_t> encode_header(const WireHeader& header) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(header.has_payload_offset ? 24 : 20);
    append_u32(bytes, wire_magic);
    append_u32(bytes, header.message_id);
    append_u32(bytes, header.body_length);
    bytes.push_back(header.channel);
    bytes.push_back(header.stream);
    append_u16(bytes, header.message_number);
    append_u16(bytes, header.response_code);
    append_u16(bytes, header.message_class);
    if (header.has_payload_offset) {
        append_u32(bytes, header.payload_offset);
    }
    return bytes;
}

bool decode_header(std::span<const std::uint8_t> bytes, WireHeader* header, std::string* error) {
    if (header == nullptr) {
        set_error(error, "Baichuan header destination is null");
        return false;
    }
    if (bytes.size() < 20) {
        set_error(error, "incomplete Baichuan header");
        return false;
    }
    if (read_u32(bytes, 0) != wire_magic) {
        set_error(error, "invalid Baichuan header magic");
        return false;
    }
    WireHeader decoded;
    decoded.message_id = read_u32(bytes, 4);
    decoded.body_length = read_u32(bytes, 8);
    decoded.channel = bytes[12];
    decoded.stream = bytes[13];
    decoded.message_number = read_u16(bytes, 14);
    decoded.response_code = read_u16(bytes, 16);
    decoded.message_class = read_u16(bytes, 18);
    decoded.has_payload_offset =
        decoded.message_class == modern_class || decoded.message_class == 0;
    if (decoded.has_payload_offset) {
        if (bytes.size() < 24) {
            set_error(error, "incomplete modern Baichuan header");
            return false;
        }
        decoded.payload_offset = read_u32(bytes, 20);
    }
    if (decoded.body_length > maximum_wire_body || decoded.payload_offset > decoded.body_length) {
        set_error(error, "invalid Baichuan body length");
        return false;
    }
    *header = decoded;
    return true;
}

std::vector<std::uint8_t> bc_xor(std::span<const std::uint8_t> bytes, std::uint32_t offset) {
    constexpr std::array<std::uint8_t, 8> key{0x1f, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78, 0xff};
    std::vector<std::uint8_t> result(bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index] =
            bytes[index] ^ key[(index + offset) % key.size()] ^ static_cast<std::uint8_t>(offset);
    }
    return result;
}

std::string md5_upper(std::string_view value) {
    std::array<std::uint8_t, 16> digest{};
    AVMD5* raw = av_md5_alloc();
    if (raw == nullptr) {
        throw std::runtime_error("cannot allocate MD5 context");
    }
    std::unique_ptr<AVMD5, decltype(&av_free)> context(raw, &av_free);
    av_md5_init(context.get());
    av_md5_update(context.get(), reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    av_md5_final(context.get(), digest.data());
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::string result;
    result.resize(digest.size() * 2U);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2U] = hexadecimal[digest[index] >> 4U];
        result[index * 2U + 1U] = hexadecimal[digest[index] & 0x0fU];
    }
    return result;
}

std::vector<std::uint8_t> aes_cfb128(std::span<const std::uint8_t> bytes,
                                     std::span<const std::uint8_t, 16> key, bool encrypt) {
    constexpr std::array<unsigned char, 16> initialization_vector{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    AVAES* raw = av_aes_alloc();
    if (raw == nullptr) {
        throw std::runtime_error("cannot allocate AES context");
    }
    std::unique_ptr<AVAES, decltype(&av_free)> context(raw, &av_free);
    if (av_aes_init(context.get(), key.data(), 128, 0) != 0) {
        throw std::runtime_error("cannot initialize AES-128");
    }
    std::array<std::uint8_t, 16> feedback = initialization_vector;
    std::array<std::uint8_t, 16> key_stream{};
    std::vector<std::uint8_t> result(bytes.size());
    for (std::size_t offset = 0; offset < bytes.size(); offset += feedback.size()) {
        av_aes_crypt(context.get(), key_stream.data(), feedback.data(), 1, nullptr, 0);
        const std::size_t count = std::min(feedback.size(), bytes.size() - offset);
        for (std::size_t index = 0; index < count; ++index) {
            result[offset + index] = bytes[offset + index] ^ key_stream[index];
        }
        if (count == feedback.size()) {
            const auto source = encrypt
                                    ? std::span<const std::uint8_t>(result.data() + offset, count)
                                    : bytes.subspan(offset, count);
            std::copy(source.begin(), source.end(), feedback.begin());
        }
    }
    return result;
}

int measured_fps(std::span<const std::uint32_t> timestamps, int reported) {
    std::vector<std::uint32_t> deltas;
    for (std::size_t index = 1; index < timestamps.size(); ++index) {
        const std::uint32_t delta = timestamps[index] - timestamps[index - 1];
        if (delta > 0 && delta <= 1'000'000) {
            deltas.push_back(delta);
        }
    }
    if (deltas.empty()) {
        return reported;
    }
    std::sort(deltas.begin(), deltas.end());
    const std::uint32_t median = deltas[deltas.size() / 2U];
    const std::uint64_t measured =
        (1'000'000ULL + static_cast<std::uint64_t>(median) / 2ULL) / median;
    if (measured < 1 || measured > 120) {
        return reported;
    }
    return static_cast<int>(measured);
}

} // namespace baichuan_detail

struct BaichuanClient::Impl {
    explicit Impl(BaichuanConfig value) : config(std::move(value)) {}

    BaichuanConfig config;
    int socket = -1;
    Encryption encryption = Encryption::none;
    std::array<std::uint8_t, 16> aes_key{};
    std::vector<std::uint8_t> wire_buffer;
    MediaParser media_parser;
    std::deque<BaichuanFrame> pending;
    BaichuanMetadata metadata;
    std::uint16_t video_message_number = 1;
    bool binary_video = false;
    bool video_started = false;

    std::uint8_t stream_code() const {
        return config.stream == BaichuanStream::sub ? 1 : 0;
    }

    std::uint32_t stream_handle() const {
        switch (config.stream) {
        case BaichuanStream::main:
            return 0;
        case BaichuanStream::sub:
            return 256;
        case BaichuanStream::external:
            return 1024;
        }
        return 0;
    }

    std::string stream_name() const {
        switch (config.stream) {
        case BaichuanStream::main:
            return "mainStream";
        case BaichuanStream::sub:
            return "subStream";
        case BaichuanStream::external:
            return "externStream";
        }
        return "mainStream";
    }

    std::vector<std::uint8_t> crypt(std::span<const std::uint8_t> bytes, Encryption mode,
                                    bool encrypt) const {
        switch (mode) {
        case Encryption::none:
            return {bytes.begin(), bytes.end()};
        case Encryption::bc_xor:
            return baichuan_detail::bc_xor(bytes, config.channel);
        case Encryption::aes:
        case Encryption::full_aes:
            return baichuan_detail::aes_cfb128(bytes, aes_key, encrypt);
        }
        return {};
    }

    bool send_packet(const baichuan_detail::WireHeader& requested_header,
                     std::span<const std::uint8_t> body, Deadline deadline, std::string* error) {
        auto header = requested_header;
        header.body_length = static_cast<std::uint32_t>(body.size());
        std::vector<std::uint8_t> bytes = baichuan_detail::encode_header(header);
        bytes.insert(bytes.end(), body.begin(), body.end());
        return send_all(socket, bytes, deadline, error);
    }

    bool send_xml(std::uint32_t message_id, std::uint16_t number, std::uint16_t response,
                  std::uint8_t stream, std::string_view xml, Encryption mode, Deadline deadline,
                  std::string* error) {
        const auto encrypted =
            crypt(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(xml.data()),
                                                xml.size()),
                  mode, true);
        baichuan_detail::WireHeader header{
            .message_id = message_id,
            .channel = config.channel,
            .stream = stream,
            .message_number = number,
            .response_code = response,
            .message_class = modern_class,
            .payload_offset = 0,
            .has_payload_offset = true,
        };
        return send_packet(header, encrypted, deadline, error);
    }

    std::optional<WirePacket> receive_packet(Deadline deadline, bool* timed_out,
                                             std::string* error) {
        if (timed_out != nullptr) {
            *timed_out = false;
        }
        for (;;) {
            if (wire_buffer.size() >= 20) {
                baichuan_detail::WireHeader header;
                std::string header_error;
                const std::size_t tentative_header =
                    (read_u16(wire_buffer, 18) == modern_class || read_u16(wire_buffer, 18) == 0)
                        ? 24
                        : 20;
                if (wire_buffer.size() >= tentative_header &&
                    !baichuan_detail::decode_header(
                        std::span<const std::uint8_t>(wire_buffer.data(), tentative_header),
                        &header, &header_error)) {
                    set_error(error, header_error);
                    return std::nullopt;
                }
                if (wire_buffer.size() >= tentative_header) {
                    const std::size_t total =
                        tentative_header + static_cast<std::size_t>(header.body_length);
                    if (wire_buffer.size() >= total) {
                        WirePacket packet;
                        packet.header = header;
                        packet.body.assign(
                            wire_buffer.begin() + static_cast<std::ptrdiff_t>(tentative_header),
                            wire_buffer.begin() + static_cast<std::ptrdiff_t>(total));
                        wire_buffer.erase(wire_buffer.begin(),
                                          wire_buffer.begin() + static_cast<std::ptrdiff_t>(total));
                        return packet;
                    }
                }
            }
            if (!wait_socket(socket, POLLIN, deadline, error)) {
                if (error == nullptr || error->empty()) {
                    if (timed_out != nullptr) {
                        *timed_out = true;
                    }
                }
                return std::nullopt;
            }
            std::array<std::uint8_t, std::size_t{64} * 1024U> chunk{};
            const ssize_t received = ::recv(socket, chunk.data(), chunk.size(), 0);
            if (received > 0) {
                wire_buffer.insert(wire_buffer.end(), chunk.begin(),
                                   chunk.begin() + static_cast<std::ptrdiff_t>(received));
                if (wire_buffer.size() > maximum_wire_body + 24U) {
                    set_error(error, "Baichuan receive buffer exceeded safety limit");
                    return std::nullopt;
                }
                continue;
            }
            if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            set_error(error, received == 0 ? "Baichuan camera closed the connection"
                                           : socket_error("cannot receive from Baichuan camera"));
            return std::nullopt;
        }
    }

    bool reply_keepalive(const WirePacket& packet, Deadline deadline, std::string* error) {
        baichuan_detail::WireHeader response{
            .message_id = keepalive_message,
            .channel = packet.header.channel,
            .stream = packet.header.stream,
            .message_number = packet.header.message_number,
            .response_code = 200,
            .message_class = modern_class,
            .payload_offset = 0,
            .has_payload_offset = true,
        };
        return send_packet(response, {}, deadline, error);
    }

    std::optional<WirePacket> wait_for(std::uint32_t message_id, std::uint16_t number,
                                       Deadline deadline, std::string* error) {
        for (;;) {
            bool timed_out = false;
            auto packet = receive_packet(deadline, &timed_out, error);
            if (!packet.has_value()) {
                if (timed_out) {
                    set_error(error, "Baichuan command timed out");
                }
                return std::nullopt;
            }
            if (packet->header.message_id == keepalive_message) {
                if (!reply_keepalive(*packet, deadline, error)) {
                    return std::nullopt;
                }
                continue;
            }
            if (packet->header.message_id == message_id &&
                packet->header.message_number == number) {
                return packet;
            }
        }
    }

    ParsedPayload parse_payload(const WirePacket& packet) {
        const std::size_t extension_size =
            packet.header.has_payload_offset ? packet.header.payload_offset : 0;
        std::string extension;
        bool binary = binary_video && packet.header.message_id == video_message &&
                      packet.header.message_number == video_message_number;
        std::optional<std::uint32_t> encrypted_length;
        if (extension_size > 0) {
            const auto decrypted =
                crypt(std::span<const std::uint8_t>(packet.body.data(), extension_size), encryption,
                      false);
            extension.assign(reinterpret_cast<const char*>(decrypted.data()), decrypted.size());
            binary = xml_unsigned(extension, "binaryData") == 1;
            encrypted_length = xml_unsigned(extension, "encryptLen");
            if (binary && packet.header.message_id == video_message &&
                packet.header.message_number == video_message_number) {
                binary_video = true;
            }
        }
        const std::span<const std::uint8_t> payload(packet.body.data() + extension_size,
                                                    packet.body.size() - extension_size);
        if (binary) {
            if (encryption == Encryption::full_aes && encrypted_length.has_value()) {
                auto decrypted = crypt(payload, encryption, false);
                if (*encrypted_length > decrypted.size()) {
                    throw std::runtime_error("invalid Baichuan encrypted media length");
                }
                decrypted.resize(*encrypted_length);
                return {true, std::move(decrypted)};
            }
            return {true, {payload.begin(), payload.end()}};
        }
        Encryption payload_encryption = encryption;
        if (packet.header.message_id == login_message) {
            payload_encryption = (packet.header.response_code & 0xff00U) == 0xdd00U &&
                                         (packet.header.response_code & 0x00ffU) == 0
                                     ? Encryption::none
                                     : Encryption::bc_xor;
        }
        return {false, crypt(payload, payload_encryption, false)};
    }

    bool login(Deadline deadline, std::string* error, bool* authentication_failed = nullptr) {
        if (authentication_failed != nullptr) {
            *authentication_failed = false;
        }
        baichuan_detail::WireHeader upgrade{
            .message_id = login_message,
            .channel = config.channel,
            .stream = 0,
            .message_number = 0,
            .response_code = 0xdc12,
            .message_class = login_upgrade_class,
            .has_payload_offset = false,
        };
        if (!send_packet(upgrade, {}, deadline, error)) {
            return false;
        }
        auto upgrade_reply = wait_for(login_message, 0, deadline, error);
        if (!upgrade_reply.has_value()) {
            return false;
        }
        const std::uint8_t negotiation =
            static_cast<std::uint8_t>(upgrade_reply->header.response_code & 0xffU);
        if ((upgrade_reply->header.response_code & 0xff00U) != 0xdd00U) {
            set_error(error, "Baichuan camera rejected encryption negotiation");
            return false;
        }
        auto negotiation_payload = parse_payload(*upgrade_reply);
        const std::string negotiation_xml(
            reinterpret_cast<const char*>(negotiation_payload.bytes.data()),
            negotiation_payload.bytes.size());
        const std::string nonce = xml_tag(negotiation_xml, "nonce");
        if (nonce.empty()) {
            set_error(error, "Baichuan encryption reply has no nonce");
            return false;
        }
        if (negotiation == 0) {
            encryption = Encryption::none;
        } else if (negotiation == 1) {
            encryption = Encryption::bc_xor;
        } else if (negotiation == 2 || negotiation == 0x11 || negotiation == 0x12) {
            const std::string key_hash = baichuan_detail::md5_upper(nonce + "-" + config.password);
            std::copy_n(key_hash.begin(), aes_key.size(), aes_key.begin());
            encryption = negotiation == 0x12 ? Encryption::full_aes : Encryption::aes;
        } else {
            set_error(error, "Baichuan camera selected an unsupported encryption mode");
            return false;
        }
        std::string username_hash =
            baichuan_detail::md5_upper(config.username + nonce).substr(0, 31);
        std::string password_hash =
            baichuan_detail::md5_upper(config.password + nonce).substr(0, 31);
        const std::string login_xml = xml_document(
            "<LoginUser version=\"1.1\"><userName>" + username_hash + "</userName><password>" +
            password_hash +
            "</password><userVer>1</userVer></LoginUser>"
            "<LoginNet version=\"1.1\"><type>LAN</type><udpPort>0</udpPort></LoginNet>");
        if (!send_xml(login_message, 0, 0, 0, login_xml, Encryption::bc_xor, deadline, error)) {
            return false;
        }
        auto login_reply = wait_for(login_message, 0, deadline, error);
        if (!login_reply.has_value()) {
            return false;
        }
        if (login_reply->header.response_code != 200) {
            if (authentication_failed != nullptr) {
                *authentication_failed = true;
            }
            set_error(error, "Baichuan authentication failed (response " +
                                 std::to_string(login_reply->header.response_code) + ")");
            return false;
        }
        return true;
    }

    bool start_video(Deadline deadline, std::string* error) {
        const std::string preview_xml =
            xml_document("<Preview version=\"1.1\"><channelId>" + std::to_string(config.channel) +
                         "</channelId><handle>" + std::to_string(stream_handle()) +
                         "</handle><streamType>" + stream_name() + "</streamType></Preview>");
        if (!send_xml(video_message, video_message_number, 0, stream_code(), preview_xml,
                      encryption, deadline, error)) {
            return false;
        }
        auto reply = wait_for(video_message, video_message_number, deadline, error);
        if (!reply.has_value()) {
            return false;
        }
        if (reply->header.response_code != 200) {
            set_error(error, "Baichuan camera rejected video start (response " +
                                 std::to_string(reply->header.response_code) + ")");
            return false;
        }
        try {
            auto payload = parse_payload(*reply);
            if (payload.binary && !payload.bytes.empty()) {
                media_parser.append(payload.bytes);
            }
        } catch (const std::exception& exception) {
            set_error(error, exception.what());
            return false;
        }
        video_started = true;
        return true;
    }

    std::optional<MediaRecord> receive_media(Deadline deadline, bool* timed_out,
                                             std::string* error) {
        for (;;) {
            if (auto record = media_parser.next(); record.has_value()) {
                return record;
            }
            auto packet = receive_packet(deadline, timed_out, error);
            if (!packet.has_value()) {
                return std::nullopt;
            }
            if (packet->header.message_id == keepalive_message) {
                if (!reply_keepalive(*packet, deadline, error)) {
                    return std::nullopt;
                }
                continue;
            }
            if (packet->header.message_id != video_message ||
                packet->header.message_number != video_message_number) {
                continue;
            }
            try {
                auto payload = parse_payload(*packet);
                if (payload.binary && !payload.bytes.empty()) {
                    media_parser.append(payload.bytes);
                }
            } catch (const std::exception& exception) {
                set_error(error, exception.what());
                return std::nullopt;
            }
        }
    }

    bool collect_initial_frames(Deadline deadline, std::string* error) {
        std::vector<std::uint32_t> timestamps;
        int reported_fps = 0;
        bool saw_keyframe = false;
        while (pending.size() < initial_frame_count) {
            bool timed_out = false;
            auto record = receive_media(deadline, &timed_out, error);
            if (!record.has_value()) {
                if (timed_out) {
                    set_error(error, "Baichuan stream metadata timed out");
                }
                return false;
            }
            if (record->type == MediaRecord::Type::information) {
                metadata.width = record->width;
                metadata.height = record->height;
                reported_fps = record->reported_fps;
                continue;
            }
            if (record->type != MediaRecord::Type::video) {
                continue;
            }
            if (!saw_keyframe) {
                if (!record->frame.keyframe) {
                    continue;
                }
                saw_keyframe = true;
                metadata.codec = record->codec;
            } else if (record->codec != metadata.codec) {
                set_error(error, "Baichuan stream changed codec during startup");
                return false;
            }
            timestamps.push_back(record->frame.timestamp_us);
            pending.push_back(std::move(record->frame));
        }
        metadata.fps = baichuan_detail::measured_fps(timestamps, reported_fps);
        return true;
    }

    void close() noexcept {
        if (socket >= 0) {
            try {
                if (video_started) {
                    const Deadline deadline = Clock::now() + std::chrono::milliseconds(150);
                    const std::string stop_xml =
                        xml_document("<Preview version=\"1.1\"><channelId>" +
                                     std::to_string(config.channel) + "</channelId><handle>" +
                                     std::to_string(stream_handle()) + "</handle></Preview>");
                    std::string ignored;
                    (void)send_xml(video_stop_message, video_message_number, 0, stream_code(),
                                   stop_xml, encryption, deadline, &ignored);
                }
            } catch (...) {
            }
            ::shutdown(socket, SHUT_RDWR);
            ::close(socket);
            socket = -1;
        }
        encryption = Encryption::none;
        wire_buffer.clear();
        media_parser = {};
        pending.clear();
        metadata = {};
        binary_video = false;
        video_started = false;
    }
};

BaichuanClient::BaichuanClient(BaichuanConfig config) : impl_(new Impl(std::move(config))) {}
BaichuanClient::~BaichuanClient() {
    if (impl_ != nullptr) {
        impl_->close();
        delete impl_;
    }
}
BaichuanClient::BaichuanClient(BaichuanClient&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)) {}
BaichuanClient& BaichuanClient::operator=(BaichuanClient&& other) noexcept {
    if (this != &other) {
        if (impl_ != nullptr) {
            impl_->close();
            delete impl_;
        }
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

BaichuanProbeResult BaichuanClient::probe(BaichuanConfig config) {
    BaichuanClient client(std::move(config));
    if (client.impl_->config.host.empty()) {
        return {BaichuanProbeStatus::unavailable, "Baichuan host is empty"};
    }
    const Deadline deadline = Clock::now() + client.impl_->config.open_timeout;
    std::string error;
    try {
        client.impl_->socket = connect_tcp(client.impl_->config, deadline, &error);
        if (client.impl_->socket < 0) {
            return {BaichuanProbeStatus::unavailable, std::move(error)};
        }
        bool authentication_failed = false;
        if (!client.impl_->login(deadline, &error, &authentication_failed)) {
            return {authentication_failed ? BaichuanProbeStatus::authentication_failed
                                          : BaichuanProbeStatus::unavailable,
                    std::move(error)};
        }
        return {BaichuanProbeStatus::available, {}};
    } catch (const std::exception& exception) {
        return {BaichuanProbeStatus::unavailable, exception.what()};
    }
}

bool BaichuanClient::open(std::string* error) {
    impl_->close();
    if (impl_->config.host.empty()) {
        set_error(error, "Baichuan host is empty");
        return false;
    }
    const Deadline deadline = Clock::now() + impl_->config.open_timeout;
    try {
        impl_->socket = connect_tcp(impl_->config, deadline, error);
        if (impl_->socket < 0) {
            return false;
        }
        if (!impl_->login(deadline, error) || !impl_->start_video(deadline, error) ||
            !impl_->collect_initial_frames(deadline, error)) {
            impl_->close();
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(error, exception.what());
        impl_->close();
        return false;
    }
}

void BaichuanClient::close() noexcept {
    if (impl_ != nullptr) {
        impl_->close();
    }
}

bool BaichuanClient::is_open() const noexcept {
    return impl_ != nullptr && impl_->socket >= 0;
}

const BaichuanMetadata& BaichuanClient::metadata() const noexcept {
    static const BaichuanMetadata empty;
    return impl_ != nullptr ? impl_->metadata : empty;
}

BaichuanReadResult BaichuanClient::read(std::chrono::milliseconds timeout) {
    if (!is_open()) {
        return {BaichuanReadStatus::error, {}, "Baichuan camera is not open"};
    }
    if (!impl_->pending.empty()) {
        BaichuanFrame frame = std::move(impl_->pending.front());
        impl_->pending.pop_front();
        return {BaichuanReadStatus::frame, std::move(frame), {}};
    }
    const Deadline deadline = Clock::now() + std::max(timeout, std::chrono::milliseconds(1));
    for (;;) {
        bool timed_out = false;
        std::string error;
        auto record = impl_->receive_media(deadline, &timed_out, &error);
        if (!record.has_value()) {
            if (timed_out) {
                return {BaichuanReadStatus::timeout, {}, {}};
            }
            return {BaichuanReadStatus::error,
                    {},
                    error.empty() ? "Baichuan media read failed" : std::move(error)};
        }
        if (record->type == MediaRecord::Type::video) {
            return {BaichuanReadStatus::frame, std::move(record->frame), {}};
        }
    }
}

} // namespace vibe_motion
