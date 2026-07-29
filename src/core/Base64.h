// Base64, for embedding binary plugin state in the JSON patch format.
//
// VST2 plugins hand back opaque state chunks. Those have to survive a text
// round trip, and base64 is the least surprising way to do that in a file people
// might open in an editor.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acm {

inline std::string base64Encode(const void* data, std::size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::string out;
    out.reserve(((size + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16)
                                   | (static_cast<std::uint32_t>(bytes[i + 1]) << 8)
                                   | bytes[i + 2];
        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += kAlphabet[(triple >> 6) & 0x3F];
        out += kAlphabet[triple & 0x3F];
    }

    if (i < size) {
        std::uint32_t triple = static_cast<std::uint32_t>(bytes[i]) << 16;
        const bool haveSecond = (i + 1) < size;
        if (haveSecond) triple |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;

        out += kAlphabet[(triple >> 18) & 0x3F];
        out += kAlphabet[(triple >> 12) & 0x3F];
        out += haveSecond ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        out += '=';
    }

    return out;
}

inline std::string base64Encode(const std::vector<std::uint8_t>& data) {
    return base64Encode(data.data(), data.size());
}

inline std::vector<std::uint8_t> base64Decode(std::string_view text) {
    const auto decodeChar = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;   // padding, whitespace, or junk
    };

    std::vector<std::uint8_t> out;
    out.reserve(text.size() * 3 / 4);

    std::uint32_t accumulator = 0;
    int bits = 0;

    for (char c : text) {
        const int value = decodeChar(c);
        // Anything unrecognised - newlines from a wrapped file, stray spaces -
        // is skipped rather than treated as an error.
        if (value < 0) continue;

        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }

    return out;
}

} // namespace acm
