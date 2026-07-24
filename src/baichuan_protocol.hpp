#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vibe_motion::baichuan_detail {

struct WireHeader {
    std::uint32_t message_id = 0;
    std::uint32_t body_length = 0;
    std::uint8_t channel = 0;
    std::uint8_t stream = 0;
    std::uint16_t message_number = 0;
    std::uint16_t response_code = 0;
    std::uint16_t message_class = 0;
    std::uint32_t payload_offset = 0;
    bool has_payload_offset = false;
};

std::vector<std::uint8_t> encode_header(const WireHeader& header);
bool decode_header(std::span<const std::uint8_t> bytes, WireHeader* header, std::string* error);
std::vector<std::uint8_t> bc_xor(std::span<const std::uint8_t> bytes, std::uint32_t offset);
std::string md5_upper(std::string_view value);
std::vector<std::uint8_t> aes_cfb128(std::span<const std::uint8_t> bytes,
                                     std::span<const std::uint8_t, 16> key, bool encrypt);
int measured_fps(std::span<const std::uint32_t> timestamps, int reported);

} // namespace vibe_motion::baichuan_detail
