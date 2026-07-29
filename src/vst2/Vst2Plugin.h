// The interface the rest of acomposter sees when it talks to a VST2 plugin.
//
// There are two implementations behind it:
//
//   NativeVst2Plugin  - the DLL is loaded into this process. Only possible when
//                       the plugin's architecture matches ours.
//   BridgedVst2Plugin - the DLL lives in a helper process of the right
//                       architecture and is driven over shared memory. This is
//                       the only way to run a 32-bit plugin from the 64-bit
//                       host, and it doubles as crash isolation for 64-bit ones.
//
// Nothing above this header knows or cares which is in use.
#pragma once

#include "../core/Json.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acm::vst2 {

enum class Architecture : std::uint8_t { Unknown = 0, X86, X64 };

const char* toString(Architecture arch) noexcept;

// Whether a plugin of this architecture can be loaded straight into this
// process, or needs the bridge.
bool isNativeArchitecture(Architecture arch) noexcept;

struct PluginDescription {
    std::string path;
    std::string name;         // effGetEffectName, falling back to the file name
    std::string vendor;
    std::string product;
    std::string category;

    std::int32_t uniqueId = 0;
    std::int32_t version = 0;
    std::int32_t vstVersion = 0;

    int numInputs = 0;
    int numOutputs = 0;
    int numParameters = 0;
    int numPrograms = 0;

    bool isSynth = false;
    bool hasEditor = false;
    bool usesChunks = false;

    Architecture architecture = Architecture::Unknown;

    // Used to invalidate the scan cache when a plugin is replaced on disk.
    std::int64_t fileSize = 0;
    std::int64_t fileModifiedTime = 0;

    // A stable key for the patch format: unique id plus the file name, so a
    // patch still finds the plugin after the user reorganises their VST folder.
    std::string identifier() const;

    bool valid() const noexcept { return !path.empty() && architecture != Architecture::Unknown; }
};

// Descriptions cross the process boundary as JSON, which keeps the fixed-layout
// bridge control block free of variable-length data, and doubles as the on-disk
// format for the scan cache.
JsonValue toJson(const PluginDescription& description);
PluginDescription pluginDescriptionFromJson(const JsonValue& in);

// The subset of transport state a plugin is told about each block.
struct HostTimeInfo {
    double samplePosition = 0.0;
    double sampleRate = 48000.0;
    double ppqPosition = 0.0;
    double tempo = 120.0;
    double barStartPosition = 0.0;
    double cycleStart = 0.0;
    double cycleEnd = 0.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    bool playing = false;
    bool recording = false;
    bool cycleActive = false;
};

struct MidiMessage {
    unsigned char status = 0;
    unsigned char data1 = 0;
    unsigned char data2 = 0;
    int deltaFrames = 0;
};

class Vst2Plugin {
public:
    virtual ~Vst2Plugin() = default;

    virtual bool valid() const = 0;
    virtual const PluginDescription& description() const = 0;
    // Set when the plugin failed or stopped responding; empty when healthy.
    virtual std::string errorText() const = 0;

    // -- lifecycle (message thread, audio stopped) -------------------------
    virtual void setSampleRateAndBlockSize(double sampleRate, int blockSize) = 0;
    virtual void setActive(bool active) = 0;

    // -- audio thread ------------------------------------------------------
    // Channel counts are the plugin's own; the node adapts around it.
    virtual void process(const float* const* inputs, int numInputs,
                         float* const* outputs, int numOutputs,
                         int frames, const HostTimeInfo& time) = 0;

    // -- parameters --------------------------------------------------------
    virtual int parameterCount() const = 0;
    virtual float parameterValue(int index) const = 0;
    virtual void setParameterValue(int index, float value) = 0;
    virtual std::string parameterName(int index) const = 0;
    virtual std::string parameterLabel(int index) const = 0;
    virtual std::string parameterDisplay(int index) const = 0;

    // -- programs ----------------------------------------------------------
    virtual int programCount() const = 0;
    virtual int currentProgram() const = 0;
    virtual void setCurrentProgram(int index) = 0;
    virtual std::string programName(int index) const = 0;

    // -- state -------------------------------------------------------------
    // Chunk-based when the plugin advertises it, otherwise a parameter dump.
    virtual std::vector<std::uint8_t> saveState() const = 0;
    virtual bool restoreState(const std::vector<std::uint8_t>& data) = 0;

    // -- editor ------------------------------------------------------------
    virtual bool hasEditor() const = 0;
    virtual bool openEditor() = 0;
    virtual void closeEditor() = 0;
    virtual bool editorOpen() const = 0;
    // Pumped once per UI frame; some plugins only redraw from here.
    virtual void idle() = 0;

    // -- midi --------------------------------------------------------------
    virtual void sendMidi(const MidiMessage& message) = 0;

    virtual int latencyFrames() const { return 0; }

    // True once when the plugin has changed its own parameters behind our back
    // - a preset loaded from its editor, a knob moved in its own window. The
    // host node re-reads everything in response. Reading clears the flag.
    virtual bool consumeParameterRefreshFlag() noexcept { return false; }
};

using Vst2PluginPtr = std::unique_ptr<Vst2Plugin>;

} // namespace acm::vst2
