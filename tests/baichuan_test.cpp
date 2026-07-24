#include "baichuan.hpp"
#include "baichuan_protocol.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace vibe_motion::baichuan_detail;

int main() {
    const WireHeader original{
        .message_id = 3,
        .body_length = 1234,
        .channel = 12,
        .stream = 1,
        .message_number = 7,
        .response_code = 200,
        .message_class = 0x6414,
        .payload_offset = 91,
        .has_payload_offset = true,
    };
    const auto encoded = encode_header(original);
    assert(encoded.size() == 24);
    assert(encoded[0] == 0xf0 && encoded[1] == 0xde && encoded[2] == 0xbc && encoded[3] == 0x0a);
    WireHeader decoded;
    std::string error;
    assert(decode_header(encoded, &decoded, &error));
    assert(decoded.message_id == original.message_id);
    assert(decoded.body_length == original.body_length);
    assert(decoded.channel == original.channel);
    assert(decoded.stream == original.stream);
    assert(decoded.message_number == original.message_number);
    assert(decoded.response_code == original.response_code);
    assert(decoded.message_class == original.message_class);
    assert(decoded.payload_offset == original.payload_offset);

    const std::array<std::uint8_t, 8> clear{0, 1, 2, 3, 4, 5, 6, 7};
    const auto encrypted_xor = bc_xor(clear, 3);
    assert(encrypted_xor != std::vector<std::uint8_t>(clear.begin(), clear.end()));
    assert(bc_xor(encrypted_xor, 3) == std::vector<std::uint8_t>(clear.begin(), clear.end()));

    assert(md5_upper("") == "D41D8CD98F00B204E9800998ECF8427E");
    assert(md5_upper("abc") == "900150983CD24FB0D6963F7D28E17F72");

    const std::array<std::uint8_t, 16> key{'0', '1', '2', '3', '4', '5', '6', '7',
                                           '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    const std::array<std::uint8_t, 19> plaintext{'B', 'a', 'i', 'c', 'h', 'u', 'a', 'n', ' ', 'A',
                                                 'E', 'S', ' ', 't', 'e', 's', 't', '!', '\n'};
    const auto ciphertext = aes_cfb128(plaintext, key, true);
    assert(ciphertext.size() == plaintext.size());
    assert(ciphertext != std::vector<std::uint8_t>(plaintext.begin(), plaintext.end()));
    assert(aes_cfb128(ciphertext, key, false) ==
           std::vector<std::uint8_t>(plaintext.begin(), plaintext.end()));

    const std::array<std::uint32_t, 9> ten_fps{4'000'000'000U, 4'000'100'000U, 4'000'200'000U,
                                               4'000'300'000U, 4'000'400'000U, 4'000'500'000U,
                                               4'000'600'000U, 4'000'700'000U, 4'000'800'000U};
    assert(measured_fps(ten_fps, 25) == 10);
    const std::array<std::uint32_t, 3> wrapping{4'294'900'000U, 3'704U, 103'704U};
    assert(measured_fps(wrapping, 25) == 10);
    const std::array<std::uint32_t, 2> unusable{10, 10};
    assert(measured_fps(unusable, 17) == 17);

    const char* host = std::getenv("VIBE_TEST_BAICHUAN_HOST");
    if (host == nullptr) {
        return 0;
    }
    const char* username = std::getenv("VIBE_TEST_BAICHUAN_USERNAME");
    const char* password = std::getenv("VIBE_TEST_BAICHUAN_PASSWORD");
    assert(username != nullptr && password != nullptr);
    vibe_motion::BaichuanClient client({
        .host = host,
        .username = username,
        .password = password,
        .open_timeout = std::chrono::seconds(20),
    });
    assert(client.open(&error));
    const auto metadata = client.metadata();
    assert(metadata.width > 0 && metadata.height > 0);
    assert(metadata.fps > 0);
    std::size_t frames = 0;
    std::size_t keyframes = 0;
    std::size_t bytes = 0;
    std::ofstream raw_stream;
    if (const char* output = std::getenv("VIBE_TEST_BAICHUAN_OUTPUT"); output != nullptr) {
        raw_stream.open(output, std::ios::binary | std::ios::trunc);
        assert(raw_stream);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline && frames < 100) {
        auto result = client.read(std::chrono::milliseconds(500));
        if (result.status == vibe_motion::BaichuanReadStatus::timeout) {
            continue;
        }
        assert(result.status == vibe_motion::BaichuanReadStatus::frame);
        assert(!result.frame.data.empty());
        ++frames;
        bytes += result.frame.data.size();
        if (raw_stream) {
            raw_stream.write(reinterpret_cast<const char*>(result.frame.data.data()),
                             static_cast<std::streamsize>(result.frame.data.size()));
            assert(raw_stream);
        }
        if (result.frame.keyframe) {
            ++keyframes;
        }
    }
    assert(frames >= 90);
    assert(keyframes > 0);
    std::cout << "codec=" << (metadata.codec == vibe_motion::BaichuanCodec::hevc ? "hevc" : "h264")
              << " width=" << metadata.width << " height=" << metadata.height
              << " fps=" << metadata.fps << " frames=" << frames << " keyframes=" << keyframes
              << " bytes=" << bytes << '\n';
}
