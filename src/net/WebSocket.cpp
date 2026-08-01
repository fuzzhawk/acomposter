#include "WebSocket.h"

#include "../core/Base64.h"
#include "Sha1.h"

#include <algorithm>

namespace acm::net::websocket {
namespace {

// The magic string from RFC 6455. It exists so that a server that echoes the
// key back cannot accidentally look like a WebSocket server to a cache or a
// proxy that is not one.
constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

} // namespace

std::string acceptKey(const std::string& clientKey) {
    const std::array<std::uint8_t, 20> digest = sha1(clientKey + kGuid);
    return base64Encode(digest.data(), digest.size());
}

std::string encode(Opcode opcode, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | static_cast<std::uint8_t>(opcode)));

    const std::size_t size = payload.size();
    if (size < 126) {
        out.push_back(static_cast<char>(size));
    } else if (size <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((size >> 8) & 0xFF));
        out.push_back(static_cast<char>(size & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> shift) & 0xFF));
    }

    out += payload;
    return out;
}

bool decode(std::string& buffer, Frame& outFrame) {
    if (buffer.size() < 2) return false;

    const auto first = static_cast<std::uint8_t>(buffer[0]);
    const auto second = static_cast<std::uint8_t>(buffer[1]);

    const bool masked = (second & 0x80) != 0;
    std::uint64_t length = second & 0x7F;
    std::size_t offset = 2;

    if (length == 126) {
        if (buffer.size() < offset + 2) return false;
        length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[offset])) << 8)
               | static_cast<std::uint8_t>(buffer[offset + 1]);
        offset += 2;
    } else if (length == 127) {
        if (buffer.size() < offset + 8) return false;
        length = 0;
        for (int i = 0; i < 8; ++i)
            length = (length << 8) | static_cast<std::uint8_t>(buffer[offset + static_cast<std::size_t>(i)]);
        offset += 8;
    }

    // A frame from a browser is always masked, and the mask is four bytes in
    // front of the payload.
    std::uint8_t mask[4] = {};
    if (masked) {
        if (buffer.size() < offset + 4) return false;
        for (int i = 0; i < 4; ++i)
            mask[i] = static_cast<std::uint8_t>(buffer[offset + static_cast<std::size_t>(i)]);
        offset += 4;
    }

    if (buffer.size() < offset + length) return false;

    outFrame.opcode = static_cast<Opcode>(first & 0x0F);
    outFrame.final = (first & 0x80) != 0;
    outFrame.payload.assign(buffer, offset, static_cast<std::size_t>(length));

    if (masked) {
        for (std::size_t i = 0; i < outFrame.payload.size(); ++i)
            outFrame.payload[i] = static_cast<char>(
                static_cast<std::uint8_t>(outFrame.payload[i]) ^ mask[i % 4]);
    }

    buffer.erase(0, offset + static_cast<std::size_t>(length));
    return true;
}

} // namespace acm::net::websocket
