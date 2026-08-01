// The WebSocket wire format, with no sockets in it.
//
// Everything here is a pure transformation on bytes: the handshake's accept
// key, and frames in and out of a buffer. Keeping it separate from the socket
// code is what makes it testable at all - a protocol bug found by a unit test
// is an afternoon, and the same bug found through a browser is a day of
// staring at a connection that closes for no stated reason.
//
// Only what a control surface needs is implemented: text and binary data
// frames, ping, pong and close. No extensions, no compression, and no
// fragmentation on the way out. Fragmentation *in* is handled, because a
// browser is free to send it and refusing would be a bug rather than a
// simplification.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace acm::net::websocket {

// The value for Sec-WebSocket-Accept, given the client's Sec-WebSocket-Key.
std::string acceptKey(const std::string& clientKey);

enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct Frame {
    Opcode opcode = Opcode::Text;
    bool final = true;
    std::string payload;
};

// Frames `payload` for sending. Server frames are never masked, which the
// standard requires rather than merely permits.
std::string encode(Opcode opcode, const std::string& payload);

// Reads one frame from the front of `buffer`.
//
// Returns false when there is not yet a whole frame, leaving `buffer`
// untouched - a socket hands over whatever arrived, and half a frame is the
// normal case rather than an error. On success the frame's bytes are removed
// from the front.
bool decode(std::string& buffer, Frame& outFrame);

} // namespace acm::net::websocket
