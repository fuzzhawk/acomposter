// Hosts a VST2 plugin loaded directly into this process.
//
// Only usable when the plugin's architecture matches the host's, which the
// caller must have established with readPeArchitecture() first. Everything else
// goes through the bridge.
#pragma once

#include "PluginEditorWindow.h"
#include "Vst2Abi.h"
#include "Vst2Plugin.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acm::vst2 {

class NativeVst2Plugin final : public Vst2Plugin {
public:
    NativeVst2Plugin();
    ~NativeVst2Plugin() override;

    // Loads and opens the plugin. Returns false and fills errorText() on
    // failure; the object is then inert but safe to destroy.
    bool load(const std::string& utf8Path, double sampleRate, int blockSize);

    // Populates a description without keeping the plugin open. Used by the
    // scanner, which runs inside a helper process so a plugin that dies on load
    // takes only the helper with it.
    static bool describe(const std::string& utf8Path, PluginDescription& out, std::string* error);

    // -- Vst2Plugin --------------------------------------------------------
    bool valid() const override { return effect_ != nullptr; }
    const PluginDescription& description() const override { return description_; }
    std::string errorText() const override { return error_; }

    void setSampleRateAndBlockSize(double sampleRate, int blockSize) override;
    void setActive(bool active) override;

    void process(const float* const* inputs, int numInputs,
                 float* const* outputs, int numOutputs,
                 int frames, const HostTimeInfo& time) override;

    int parameterCount() const override;
    float parameterValue(int index) const override;
    void setParameterValue(int index, float value) override;
    std::string parameterName(int index) const override;
    std::string parameterLabel(int index) const override;
    std::string parameterDisplay(int index) const override;

    int programCount() const override;
    int currentProgram() const override;
    void setCurrentProgram(int index) override;
    std::string programName(int index) const override;

    std::vector<std::uint8_t> saveState() const override;
    bool restoreState(const std::vector<std::uint8_t>& data) override;

    bool hasEditor() const override;
    bool openEditor() override;
    void closeEditor() override;
    bool editorOpen() const override { return editorOpen_; }
    void idle() override;

    void sendMidi(const MidiMessage& message) override;

    int latencyFrames() const override;

    bool consumeParameterRefreshFlag() noexcept override {
        return parametersChanged_.exchange(false, std::memory_order_acquire);
    }

private:
    friend VstIntPtr hostCallbackTrampoline(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);

    VstIntPtr hostCallback(VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt);
    VstIntPtr dispatch(VstInt32 opcode, VstInt32 index = 0, VstIntPtr value = 0,
                       void* ptr = nullptr, float opt = 0.0f) const;

    std::string dispatchString(VstInt32 opcode, VstInt32 index = 0) const;

    void unload();
    void rebuildChannelPointers(int numInputs, int numOutputs, int blockSize);

    void* module_ = nullptr;      // HMODULE
    AEffect* effect_ = nullptr;
    PluginDescription description_;
    std::string error_;

    double sampleRate_ = 48000.0;
    int blockSize_ = 512;
    bool active_ = false;
    bool editorOpen_ = false;

    PluginEditorWindow editorWindow_;

    // Channel pointer arrays and silent/scratch buffers handed to
    // processReplacing. Allocated up front so process() never touches the heap.
    std::vector<float*> inputPointers_;
    std::vector<float*> outputPointers_;
    std::vector<float> silence_;
    std::vector<float> scratch_;

    // MIDI staged from the message thread and flushed at the top of the block.
    static constexpr int kMidiCapacity = 128;
    VstEventBlock<kMidiCapacity> midiEvents_;
    std::mutex midiMutex_;
    std::vector<MidiMessage> pendingMidi_;

    // Written by the audio thread, read by the plugin during its callback.
    VstTimeInfo timeInfo_{};

    std::atomic<bool> parametersChanged_{ false };
    mutable std::atomic<int> editorWidth_{ 0 };
    mutable std::atomic<int> editorHeight_{ 0 };
};

} // namespace acm::vst2
