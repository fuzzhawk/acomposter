// Shared-memory protocol between the host and a plugin bridge process.
//
// This header is compiled into binaries of *different architectures* - the
// 64-bit host and the 32-bit helper - and both map the same block of memory. So
// the layout has to be identical on both sides, which rules out anything whose
// size or alignment depends on the pointer width:
//
//   * no pointers, no size_t, no intptr_t, no bool, no enums without a fixed
//     underlying type;
//   * every 8-byte field sits at an 8-byte-aligned offset by construction, with
//     padding written out explicitly rather than left to the compiler;
//   * the static_asserts at the bottom fail the build if either compiler ever
//     disagrees, which is a far better outcome than a silent mismatch that only
//     shows up as garbled audio.
//
// Anything variable-length (strings, plugin state chunks, the JSON description)
// travels through the data area rather than the control block.
#pragma once

#include <cstddef>
#include <cstdint>

namespace acm::vst2::bridge {

inline constexpr std::uint32_t kMagic = 0x41435042;      // 'ACPB'
inline constexpr std::uint32_t kProtocolVersion = 1;

inline constexpr std::int32_t kMaxChannels = 32;
inline constexpr std::int32_t kMaxBlockSize = 8192;
inline constexpr std::int32_t kMaxMidiEvents = 64;

inline constexpr std::uint32_t kControlBytes = 4096;
inline constexpr std::uint32_t kAudioBytesPerDirection =
    static_cast<std::uint32_t>(kMaxChannels) * static_cast<std::uint32_t>(kMaxBlockSize) * 4u;
inline constexpr std::uint32_t kDataBytes = 32u << 20;   // room for a large state chunk

inline constexpr std::uint32_t kAudioInputOffset = kControlBytes;
inline constexpr std::uint32_t kAudioOutputOffset = kAudioInputOffset + kAudioBytesPerDirection;
inline constexpr std::uint32_t kDataOffset = kAudioOutputOffset + kAudioBytesPerDirection;
inline constexpr std::uint32_t kTotalBytes = kDataOffset + kDataBytes;

enum class Command : std::uint32_t {
    None = 0,
    Ping,
    Describe,          // data area receives the description as JSON
    Load,              // data area holds the UTF-8 path
    Unload,
    SetRateAndBlock,   // doubleArgs[0] = sample rate, intArgs[0] = block size
    SetActive,         // intArgs[0] = 0 or 1
    Process,           // intArgs[0] = frames
    SetParameter,      // intArgs[0] = index, doubleArgs[0] = value
    GetParameter,      // intArgs[0] = index -> doubleArgs[0]
    GetParameterInfo,  // intArgs[0] = index -> JSON in the data area
    SetProgram,        // intArgs[0] = index
    GetProgram,        // -> intArgs[0]
    GetProgramName,    // intArgs[0] = index -> data area
    GetState,          // -> data area
    SetState,          // data area holds the state
    OpenEditor,
    CloseEditor,
    EditorIdle,
    SendMidi,          // uses the midi array
    Shutdown,
};

enum class Status : std::uint32_t {
    Idle = 0,
    Ok,
    Error,             // data area holds the message
    Unsupported,
};

enum TransportFlags : std::uint32_t {
    kPlaying = 1u << 0,
    kRecording = 1u << 1,
    kCycleActive = 1u << 2,
};

// Flags the bridge raises without being asked, polled by the host each block.
enum NotifyFlags : std::uint32_t {
    kNotifyParametersChanged = 1u << 0,
    kNotifyEditorClosed      = 1u << 1,
    kNotifyEditorResized     = 1u << 2,
};

#pragma pack(push, 8)

struct MidiEvent {
    std::uint8_t status;
    std::uint8_t data1;
    std::uint8_t data2;
    std::uint8_t padding;
    std::int32_t deltaFrames;
};

struct ControlBlock {
    // -- identification (offset 0) ----------------------------------------
    std::uint32_t magic;
    std::uint32_t protocolVersion;

    // -- request / response (offset 8) ------------------------------------
    std::uint32_t command;
    std::uint32_t status;
    std::uint64_t sequence;

    // -- scalar arguments (offset 24) -------------------------------------
    std::int32_t intArgs[8];
    std::uint32_t dataSize;       // bytes valid in the data area
    std::uint32_t padding0;
    double doubleArgs[4];

    // -- audio configuration (offset 96) ----------------------------------
    std::int32_t pluginInputs;
    std::int32_t pluginOutputs;
    std::int32_t blockSize;
    std::int32_t frames;
    double sampleRate;

    // -- transport (offset 112) -------------------------------------------
    double samplePosition;
    double ppqPosition;
    double tempo;
    double barStartPosition;
    double cycleStart;
    double cycleEnd;
    std::int32_t timeSigNumerator;
    std::int32_t timeSigDenominator;
    std::uint32_t transportFlags;

    // -- notifications from the bridge (offset 172) -----------------------
    std::uint32_t notifyFlags;
    std::int32_t editorWidth;
    std::int32_t editorHeight;

    // -- midi (offset 184) -------------------------------------------------
    std::int32_t midiCount;
    std::uint32_t padding1;
    MidiEvent midi[kMaxMidiEvents];
};

#pragma pack(pop)

// If either of these fires, the two architectures have stopped agreeing on the
// layout and the bridge would silently corrupt itself.
static_assert(sizeof(MidiEvent) == 8, "bridge MidiEvent must be 8 bytes on both architectures");
static_assert(offsetof(ControlBlock, command) == 8, "bridge layout drift");
static_assert(offsetof(ControlBlock, sequence) == 16, "bridge layout drift");
static_assert(offsetof(ControlBlock, intArgs) == 24, "bridge layout drift");
static_assert(offsetof(ControlBlock, doubleArgs) == 64, "bridge layout drift");
static_assert(offsetof(ControlBlock, pluginInputs) == 96, "bridge layout drift");
static_assert(offsetof(ControlBlock, sampleRate) == 112, "bridge layout drift");
static_assert(offsetof(ControlBlock, samplePosition) == 120, "bridge layout drift");
static_assert(offsetof(ControlBlock, timeSigNumerator) == 168, "bridge layout drift");
static_assert(offsetof(ControlBlock, notifyFlags) == 180, "bridge layout drift");
static_assert(offsetof(ControlBlock, midiCount) == 192, "bridge layout drift");
static_assert(offsetof(ControlBlock, midi) == 200, "bridge layout drift");
static_assert(sizeof(ControlBlock) <= kControlBytes, "control block does not fit its region");

// Names for the shared objects. The host generates a unique suffix per instance
// and passes it to the helper on the command line.
inline constexpr const char* kSharedMemoryPrefix = "Local\\acomposter-bridge-shm-";
inline constexpr const char* kRequestEventPrefix = "Local\\acomposter-bridge-req-";
inline constexpr const char* kResponseEventPrefix = "Local\\acomposter-bridge-rsp-";

// Timeouts in milliseconds. Process has to be tight enough that a wedged plugin
// does not stall the audio device for long, and loose enough that a plugin doing
// real work on a busy machine is not declared dead.
inline constexpr std::uint32_t kProcessTimeoutMs = 250;
inline constexpr std::uint32_t kLoadTimeoutMs = 30000;
inline constexpr std::uint32_t kDefaultTimeoutMs = 5000;

} // namespace acm::vst2::bridge
