// Drives a plugin that lives in a separate helper process.
//
// This is what makes 32-bit VST2 support possible from a 64-bit host: the
// architectures cannot share an address space, so the plugin runs inside
// acomposter-bridge32.exe and everything travels through a shared-memory
// mailbox. The same mechanism runs 64-bit plugins out of process when isolation
// is wanted, using acomposter-bridge64.exe.
//
// Threading contract
// ------------------
// setParameterValue() and process() are called from the audio thread only.
// Parameter changes are batched into the Process round trip rather than each
// costing an IPC hop, which matters when the metasurface is sweeping dozens of
// plugin parameters at frame rate.
#pragma once

#include "BridgeProtocol.h"
#include "Vst2Plugin.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace acm::vst2 {

class BridgedVst2Plugin final : public Vst2Plugin {
public:
    BridgedVst2Plugin();
    ~BridgedVst2Plugin() override;

    // Starts a helper of the right architecture and loads the plugin into it.
    bool load(const std::string& utf8Path, Architecture architecture,
              double sampleRate, int blockSize);

    // Scans a plugin without keeping it running. A plugin that crashes on load
    // takes the helper down and is reported as bad, rather than taking the whole
    // application with it - which is the main reason scanning is done this way.
    static bool describe(const std::string& utf8Path, Architecture architecture,
                         PluginDescription& out, std::string* error);

    // Where the helper executables live; defaults to the host's own directory.
    static void setHelperDirectory(std::string directory);
    static std::string helperPathFor(Architecture architecture);

    // Tears down and restarts the helper, reloading the plugin and its state.
    // Offered in the UI after a plugin has crashed.
    bool relaunch();

    bool bridgeAlive() const noexcept { return alive_.load(std::memory_order_relaxed); }

    // -- Vst2Plugin --------------------------------------------------------
    bool valid() const override { return loaded_ && alive_.load(std::memory_order_relaxed); }
    const PluginDescription& description() const override { return description_; }
    std::string errorText() const override;

    void setSampleRateAndBlockSize(double sampleRate, int blockSize) override;
    void setActive(bool active) override;

    void process(const float* const* inputs, int numInputs,
                 float* const* outputs, int numOutputs,
                 int frames, const HostTimeInfo& time) override;

    int parameterCount() const override { return description_.numParameters; }
    float parameterValue(int index) const override;
    void setParameterValue(int index, float value) override;
    std::string parameterName(int index) const override;
    std::string parameterLabel(int index) const override;
    std::string parameterDisplay(int index) const override;

    int programCount() const override { return description_.numPrograms; }
    int currentProgram() const override;
    void setCurrentProgram(int index) override;
    std::string programName(int index) const override;

    std::vector<std::uint8_t> saveState() const override;
    bool restoreState(const std::vector<std::uint8_t>& data) override;

    bool hasEditor() const override { return description_.hasEditor; }
    bool openEditor() override;
    void closeEditor() override;
    bool editorOpen() const override { return editorOpen_; }
    void idle() override;

    void sendMidi(const MidiMessage& message) override;

    int latencyFrames() const override { return latency_; }

    bool consumeParameterRefreshFlag() noexcept override {
        return parametersChanged_.exchange(false, std::memory_order_acquire);
    }

private:
    struct PendingParameter {
        std::int32_t index;
        float value;
    };

    bool startHelper(Architecture architecture, std::string* error);
    void stopHelper();

    // Sends a command and waits for the reply. Returns false on error or
    // timeout; a timeout marks the bridge dead.
    bool transact(bridge::Command command, std::uint32_t timeoutMs) const;

    bridge::ControlBlock* control() const;
    float* audioInput(int channel) const;
    float* audioOutput(int channel) const;
    std::uint8_t* dataArea() const;

    void writeData(const void* data, std::size_t size) const;
    std::string readDataAsString() const;
    std::vector<std::uint8_t> readDataAsBytes() const;

    void markDead(const std::string& reason) const;
    void refreshParameterCache();

    // Handles are void* so this header does not drag windows.h everywhere.
    void* mapping_ = nullptr;
    void* view_ = nullptr;
    void* requestEvent_ = nullptr;
    void* responseEvent_ = nullptr;
    void* processHandle_ = nullptr;

    PluginDescription description_;
    Architecture architecture_ = Architecture::Unknown;
    std::string path_;
    std::string instanceKey_;

    bool loaded_ = false;
    bool editorOpen_ = false;
    int latency_ = 0;
    double sampleRate_ = 48000.0;
    int blockSize_ = 512;
    bool active_ = false;

    mutable std::atomic<bool> alive_{ false };
    mutable std::mutex errorMutex_;
    mutable std::string error_;
    mutable std::uint64_t sequence_ = 0;

    // Audio-thread-owned; drained into each Process round trip.
    std::vector<PendingParameter> pendingParameters_;
    std::vector<bridge::MidiEvent> pendingMidi_;
    std::mutex midiMutex_;

    // Mirror of the plugin's parameter values so the UI can read them without
    // an IPC hop per control per frame.
    std::vector<float> parameterCache_;
    std::atomic<bool> parametersChanged_{ false };

    // Consecutive Process timeouts before the bridge is declared dead. One
    // late block during a plugin's own preset load is not a crash.
    static constexpr int kMaxConsecutiveTimeouts = 8;
    mutable int consecutiveTimeouts_ = 0;
};

} // namespace acm::vst2
