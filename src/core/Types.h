// acomposter - core type vocabulary
//
// Everything downstream (graph, nodes, metasurface, patch format) agrees on the
// identifiers declared here. Ids are opaque 32-bit handles rather than pointers
// so that they survive serialisation and can be passed to the audio thread
// inside plain-old-data command structs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace acm {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

using NodeId = std::uint32_t;
using PortIndex = std::int32_t;
using ParamIndex = std::int32_t;
using ConnectionId = std::uint32_t;
using SnapshotId = std::uint32_t;

inline constexpr NodeId kInvalidNode = 0;
inline constexpr ConnectionId kInvalidConnection = 0;
inline constexpr SnapshotId kInvalidSnapshot = 0;
inline constexpr PortIndex kInvalidPort = -1;

// A graph-wide parameter address. Packs into a single 64-bit word so the
// metasurface and the automation system can key maps on it cheaply.
struct ParamAddress {
    NodeId node = kInvalidNode;
    ParamIndex param = -1;

    constexpr bool valid() const noexcept { return node != kInvalidNode && param >= 0; }

    constexpr std::uint64_t key() const noexcept {
        return (static_cast<std::uint64_t>(node) << 32)
             | static_cast<std::uint32_t>(param);
    }

    static constexpr ParamAddress fromKey(std::uint64_t k) noexcept {
        return ParamAddress{ static_cast<NodeId>(k >> 32),
                             static_cast<ParamIndex>(static_cast<std::uint32_t>(k)) };
    }

    friend constexpr bool operator==(const ParamAddress& a, const ParamAddress& b) noexcept {
        return a.node == b.node && a.param == b.param;
    }
    friend constexpr bool operator<(const ParamAddress& a, const ParamAddress& b) noexcept {
        return a.key() < b.key();
    }
};

struct ParamAddressHash {
    std::size_t operator()(const ParamAddress& a) const noexcept {
        // splitmix64 finaliser - cheap and well distributed for dense ids.
        std::uint64_t x = a.key() + 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return static_cast<std::size_t>(x ^ (x >> 31));
    }
};

// ---------------------------------------------------------------------------
// Engine limits
//
// These are hard ceilings used to size fixed allocations that the audio thread
// touches. They are generous but bounded on purpose: nothing in the render path
// is allowed to allocate.
// ---------------------------------------------------------------------------

inline constexpr int kMaxBlockSize = 8192;
inline constexpr int kMaxChannelsPerPort = 8;
inline constexpr int kMaxPortsPerNode = 32;
inline constexpr int kMaxNodes = 1024;
inline constexpr double kMinSampleRate = 8000.0;
inline constexpr double kMaxSampleRate = 384000.0;

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

enum class PortDirection : std::uint8_t { Input, Output };

struct PortDescriptor {
    std::string name;
    int channels = 2;
    // A "sidechain" port is still audio, but the UI draws it dimmed and the
    // auto-connect helper never picks it as a default target.
    bool sidechain = false;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

template <typename T>
constexpr T clampValue(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr float lerpf(float a, float b, float t) noexcept { return a + (b - a) * t; }
constexpr double lerpd(double a, double b, double t) noexcept { return a + (b - a) * t; }

} // namespace acm
