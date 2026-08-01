// SHA-1, for exactly one purpose: the WebSocket opening handshake.
//
// RFC 6455 requires the server to answer a connection with the base64 of the
// SHA-1 of the client's key concatenated with a fixed GUID. There is no way to
// accept a WebSocket without it, and there is no other place in this program
// that hashes anything.
//
// It is worth saying plainly that this is not here as a security primitive.
// SHA-1 is broken for signatures and nobody should reach for this expecting
// otherwise; the handshake uses it as a fixed transformation that proves both
// ends speak the protocol, and the standard names SHA-1 specifically.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace acm::net {

inline std::array<std::uint8_t, 20> sha1(std::string_view input) {
    std::uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    const auto rotate = [](std::uint32_t value, int bits) {
        return (value << bits) | (value >> (32 - bits));
    };

    // Message, padded: a 0x80 byte, zeros, then the length in bits big-endian.
    const std::uint64_t bitLength = static_cast<std::uint64_t>(input.size()) * 8;
    std::size_t padded = input.size() + 1;
    while (padded % 64 != 56) ++padded;
    padded += 8;

    const auto byteAt = [&](std::size_t index) -> std::uint8_t {
        if (index < input.size()) return static_cast<std::uint8_t>(input[index]);
        if (index == input.size()) return 0x80;
        if (index < padded - 8) return 0;
        const int shift = static_cast<int>(7 - (index - (padded - 8))) * 8;
        return static_cast<std::uint8_t>((bitLength >> shift) & 0xFF);
    };

    std::uint32_t w[80];

    for (std::size_t chunk = 0; chunk < padded; chunk += 64) {
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(byteAt(chunk + static_cast<std::size_t>(i) * 4)) << 24)
                 | (static_cast<std::uint32_t>(byteAt(chunk + static_cast<std::size_t>(i) * 4 + 1)) << 16)
                 | (static_cast<std::uint32_t>(byteAt(chunk + static_cast<std::size_t>(i) * 4 + 2)) << 8)
                 | (static_cast<std::uint32_t>(byteAt(chunk + static_cast<std::size_t>(i) * 4 + 3)));
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }

            const std::uint32_t temp = rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotate(b, 30);
            b = a;
            a = temp;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    std::array<std::uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        digest[static_cast<std::size_t>(i) * 4]     = static_cast<std::uint8_t>(h[i] >> 24);
        digest[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        digest[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        digest[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return digest;
}

} // namespace acm::net
