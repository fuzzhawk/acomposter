#include "NativeVst2Plugin.h"

#include "../core/FileIo.h"
#include "../core/Types.h"
#include "../core/Utf.h"
#include "PeArchitecture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <windows.h>

namespace acm::vst2 {
namespace {

using EntryPointProc = AEffect* (*)(AudioMasterCallback);

// The names a VST2 DLL might export its entry point under, newest first.
constexpr const char* kEntryPointNames[] = { "VSTPluginMain", "main", "main_plugin" };

// A plugin's entry point calls back into the host *before* it returns, so there
// is no AEffect to look the instance up from yet. This holds the instance being
// constructed on this thread for the duration of that call.
thread_local NativeVst2Plugin* g_constructingPlugin = nullptr;

constexpr int kStringBufferSize = 256;   // the ABI promises 64; plugins overrun

std::string vendorString() { return "acomposter"; }
std::string productString() { return "acomposter"; }

} // namespace

// ---------------------------------------------------------------------------
// Host callback
// ---------------------------------------------------------------------------

VstIntPtr hostCallbackTrampoline(AEffect* effect, VstInt32 opcode, VstInt32 index,
                                 VstIntPtr value, void* ptr, float opt) {
    NativeVst2Plugin* plugin = nullptr;

    if (effect != nullptr && effect->reservedForHost1 != 0)
        plugin = reinterpret_cast<NativeVst2Plugin*>(effect->reservedForHost1);
    else
        plugin = g_constructingPlugin;

    if (plugin == nullptr) {
        // A callback we cannot attribute. Answer only the questions that have a
        // safe universal answer.
        switch (opcode) {
            case audioMasterVersion: return kVstVersion;
            case audioMasterCurrentId: return 0;
            default: return 0;
        }
    }

    return plugin->hostCallback(opcode, index, value, ptr, opt);
}

VstIntPtr NativeVst2Plugin::hostCallback(VstInt32 opcode, VstInt32 index,
                                         VstIntPtr value, void* ptr, float opt) {
    (void)opt;

    switch (opcode) {
        case audioMasterVersion:
            return kVstVersion;

        case audioMasterCurrentId:
            return description_.uniqueId;

        case audioMasterGetSampleRate:
            return static_cast<VstIntPtr>(sampleRate_);

        case audioMasterGetBlockSize:
            return blockSize_;

        case audioMasterGetTime:
            return reinterpret_cast<VstIntPtr>(&timeInfo_);

        case audioMasterGetVendorString:
            if (ptr) std::snprintf(static_cast<char*>(ptr), 64, "%s", vendorString().c_str());
            return 1;

        case audioMasterGetProductString:
            if (ptr) std::snprintf(static_cast<char*>(ptr), 64, "%s", productString().c_str());
            return 1;

        case audioMasterGetVendorVersion:
            return 100;

        case audioMasterAutomate:
        case audioMasterBeginEdit:
        case audioMasterEndEdit:
            // The plugin moved one of its own controls. Flag it so the node
            // re-reads every parameter on the next UI frame rather than trying
            // to service an automation write from inside the plugin's callback.
            parametersChanged_.store(true, std::memory_order_release);
            return 1;

        case audioMasterUpdateDisplay:
        case audioMasterIOChanged:
            parametersChanged_.store(true, std::memory_order_release);
            return 1;

        case audioMasterSizeWindow:
            editorWidth_.store(static_cast<int>(index), std::memory_order_relaxed);
            editorHeight_.store(static_cast<int>(value), std::memory_order_relaxed);
            if (editorOpen_) editorWindow_.resizeClient(static_cast<int>(index), static_cast<int>(value));
            return 1;

        case audioMasterGetCurrentProcessLevel:
            // 2 = realtime audio thread. Close enough for the plugins that ask.
            return 2;

        case audioMasterGetAutomationState:
            return 0;   // unsupported / off

        case audioMasterGetLanguage:
            return 1;   // English

        case audioMasterGetDirectory:
            return 0;

        case audioMasterCanDo: {
            if (ptr == nullptr) return 0;
            const std::string what(static_cast<const char*>(ptr));
            // Be honest: claiming support we do not have makes plugins take
            // code paths that then fail in less obvious ways.
            if (what == "sendVstEvents" || what == "sendVstMidiEvent"
                || what == "sizeWindow" || what == "supplyIdle"
                || what == "sendVstTimeInfo" || what == "startStopProcess")
                return 1;
            return 0;
        }

        case audioMasterIdle:
            return 1;

        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

NativeVst2Plugin::NativeVst2Plugin() = default;

NativeVst2Plugin::~NativeVst2Plugin() {
    unload();
}

VstIntPtr NativeVst2Plugin::dispatch(VstInt32 opcode, VstInt32 index, VstIntPtr value,
                                     void* ptr, float opt) const {
    if (effect_ == nullptr || effect_->dispatcher == nullptr) return 0;
    return effect_->dispatcher(effect_, opcode, index, value, ptr, opt);
}

std::string NativeVst2Plugin::dispatchString(VstInt32 opcode, VstInt32 index) const {
    // Plugins are notorious for writing past the 64 bytes the ABI specifies, so
    // the buffer is oversized and zeroed.
    char buffer[kStringBufferSize];
    std::memset(buffer, 0, sizeof(buffer));
    dispatch(opcode, index, 0, buffer, 0.0f);
    return fromVstString(buffer, sizeof(buffer));
}

bool NativeVst2Plugin::load(const std::string& utf8Path, double sampleRate, int blockSize) {
    unload();

    error_.clear();
    description_ = PluginDescription{};
    description_.path = utf8Path;
    description_.name = pathStem(utf8Path);

    std::string archError;
    description_.architecture = readPeArchitecture(utf8Path, &archError);
    if (description_.architecture == Architecture::Unknown) {
        error_ = "could not read plugin architecture: " + archError;
        return false;
    }
    if (!isNativeArchitecture(description_.architecture)) {
        error_ = std::string("this is a ") + toString(description_.architecture)
               + " plugin and cannot be loaded into a "
               + (sizeof(void*) == 8 ? "64-bit" : "32-bit") + " process; use the bridge";
        return false;
    }

    description_.fileSize = fileSize(utf8Path);

    // Loading with the plugin's own folder on the search path: many plugins ship
    // their resources and helper DLLs beside themselves and will not start
    // otherwise.
    const std::wstring widePath = utf8ToWide(utf8Path);

    HMODULE module = ::LoadLibraryExW(widePath.c_str(), nullptr,
                                      LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        const DWORD code = ::GetLastError();
        error_ = "LoadLibrary failed (" + std::to_string(code) + ")";
        return false;
    }
    module_ = module;

    EntryPointProc entryPoint = nullptr;
    for (const char* name : kEntryPointNames) {
        entryPoint = reinterpret_cast<EntryPointProc>(
            reinterpret_cast<void*>(::GetProcAddress(module, name)));
        if (entryPoint) break;
    }

    if (!entryPoint) {
        error_ = "no VST2 entry point (VSTPluginMain or main) exported";
        unload();
        return false;
    }

    sampleRate_ = sampleRate;
    blockSize_ = blockSize;

    g_constructingPlugin = this;
    AEffect* effect = entryPoint(&hostCallbackTrampoline);
    g_constructingPlugin = nullptr;

    if (effect == nullptr) {
        error_ = "the plugin's entry point returned nothing";
        unload();
        return false;
    }
    if (effect->magic != kEffectMagic) {
        error_ = "the plugin returned a structure that is not a VST2 effect";
        unload();
        return false;
    }

    effect_ = effect;
    // The ABI reserves this field for the host; it is how the callback finds us.
    effect_->reservedForHost1 = reinterpret_cast<VstIntPtr>(this);

    dispatch(effOpen);
    dispatch(effSetSampleRate, 0, 0, nullptr, static_cast<float>(sampleRate_));
    dispatch(effSetBlockSize, 0, blockSize_);

    description_.uniqueId = effect_->uniqueID;
    description_.version = effect_->version;
    description_.vstVersion = static_cast<std::int32_t>(dispatch(effGetVstVersion));
    description_.numInputs = effect_->numInputs;
    description_.numOutputs = effect_->numOutputs;
    description_.numParameters = effect_->numParams;
    description_.numPrograms = effect_->numPrograms;
    description_.isSynth = (effect_->flags & effFlagsIsSynth) != 0;
    description_.hasEditor = (effect_->flags & effFlagsHasEditor) != 0;
    description_.usesChunks = (effect_->flags & effFlagsProgramChunks) != 0;

    const std::string effectName = dispatchString(effGetEffectName);
    if (!effectName.empty()) description_.name = effectName;
    description_.vendor = dispatchString(effGetVendorString);
    description_.product = dispatchString(effGetProductString);

    if ((effect_->flags & effFlagsCanReplacing) == 0) {
        error_ = "this plugin predates VST 2.4 and cannot process in place";
        unload();
        return false;
    }

    rebuildChannelPointers(description_.numInputs, description_.numOutputs, blockSize_);
    return true;
}

void NativeVst2Plugin::unload() {
    if (editorOpen_) closeEditor();

    if (effect_ != nullptr) {
        if (active_) {
            dispatch(effStopProcess);
            dispatch(effMainsChanged, 0, 0);
            active_ = false;
        }
        dispatch(effClose);
        effect_ = nullptr;
    }

    if (module_ != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

void NativeVst2Plugin::rebuildChannelPointers(int numInputs, int numOutputs, int blockSize) {
    numInputs = std::max(0, numInputs);
    numOutputs = std::max(0, numOutputs);
    blockSize = std::max(16, blockSize);

    // One shared silent buffer feeds every input the patch does not supply, and
    // a scratch buffer per output absorbs channels the patch does not consume.
    silence_.assign(static_cast<std::size_t>(blockSize), 0.0f);
    scratch_.assign(static_cast<std::size_t>(blockSize)
                        * static_cast<std::size_t>(std::max(numInputs, numOutputs) + 1),
                    0.0f);

    inputPointers_.assign(static_cast<std::size_t>(numInputs), nullptr);
    outputPointers_.assign(static_cast<std::size_t>(numOutputs), nullptr);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void NativeVst2Plugin::setSampleRateAndBlockSize(double sampleRate, int blockSize) {
    if (!effect_) return;

    const bool wasActive = active_;
    if (wasActive) setActive(false);

    sampleRate_ = sampleRate;
    blockSize_ = blockSize;

    dispatch(effSetSampleRate, 0, 0, nullptr, static_cast<float>(sampleRate_));
    dispatch(effSetBlockSize, 0, blockSize_);
    rebuildChannelPointers(description_.numInputs, description_.numOutputs, blockSize_);

    if (wasActive) setActive(true);
}

void NativeVst2Plugin::setActive(bool active) {
    if (!effect_ || active == active_) return;

    if (active) {
        dispatch(effMainsChanged, 0, 1);
        dispatch(effStartProcess);
    } else {
        dispatch(effStopProcess);
        dispatch(effMainsChanged, 0, 0);
    }
    active_ = active;
}

int NativeVst2Plugin::latencyFrames() const {
    return effect_ ? effect_->initialDelay : 0;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void NativeVst2Plugin::process(const float* const* inputs, int numInputs,
                               float* const* outputs, int numOutputs,
                               int frames, const HostTimeInfo& time) {
    if (!effect_ || !active_ || effect_->processReplacing == nullptr || frames <= 0) {
        for (int c = 0; c < numOutputs; ++c)
            std::memset(outputs[c], 0, sizeof(float) * static_cast<std::size_t>(frames));
        return;
    }

    // Refresh the time info the plugin will read back through audioMasterGetTime
    // during processReplacing.
    timeInfo_.samplePos = time.samplePosition;
    timeInfo_.sampleRate = time.sampleRate;
    timeInfo_.ppqPos = time.ppqPosition;
    timeInfo_.tempo = time.tempo;
    timeInfo_.barStartPos = time.barStartPosition;
    timeInfo_.cycleStartPos = time.cycleStart;
    timeInfo_.cycleEndPos = time.cycleEnd;
    timeInfo_.timeSigNumerator = time.timeSigNumerator;
    timeInfo_.timeSigDenominator = time.timeSigDenominator;
    timeInfo_.flags = kVstPpqPosValid | kVstTempoValid | kVstBarsValid
                    | kVstTimeSigValid | kVstCyclePosValid;
    if (time.playing) timeInfo_.flags |= kVstTransportPlaying;
    if (time.recording) timeInfo_.flags |= kVstTransportRecording;
    if (time.cycleActive) timeInfo_.flags |= kVstTransportCycleActive;

    // Flush any MIDI staged since the last block.
    {
        std::unique_lock<std::mutex> lock(midiMutex_, std::try_to_lock);
        if (lock.owns_lock() && !pendingMidi_.empty()) {
            midiEvents_.clear();
            for (const MidiMessage& message : pendingMidi_)
                midiEvents_.add(message.status, message.data1, message.data2,
                                clampValue(message.deltaFrames, 0, frames - 1));
            pendingMidi_.clear();
            lock.unlock();

            if (midiEvents_.numEvents > 0)
                dispatch(effProcessEvents, 0, 0, midiEvents_.asVstEvents());
        }
    }

    // The plugin's channel count rarely matches the patch's. Unsupplied inputs
    // read silence; unwanted outputs are written into scratch and discarded.
    const int pluginInputs = static_cast<int>(inputPointers_.size());
    const int pluginOutputs = static_cast<int>(outputPointers_.size());

    for (int c = 0; c < pluginInputs; ++c) {
        inputPointers_[static_cast<std::size_t>(c)] =
            (c < numInputs && inputs[c] != nullptr)
                ? const_cast<float*>(inputs[c])
                : silence_.data();
    }

    for (int c = 0; c < pluginOutputs; ++c) {
        outputPointers_[static_cast<std::size_t>(c)] =
            (c < numOutputs && outputs[c] != nullptr)
                ? outputs[c]
                : scratch_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(blockSize_);
    }

    effect_->processReplacing(effect_,
                              inputPointers_.empty() ? nullptr : inputPointers_.data(),
                              outputPointers_.empty() ? nullptr : outputPointers_.data(),
                              frames);

    // A plugin with fewer outputs than the patch expects leaves the extra
    // channels untouched; silence them rather than letting stale audio through.
    for (int c = pluginOutputs; c < numOutputs; ++c)
        std::memset(outputs[c], 0, sizeof(float) * static_cast<std::size_t>(frames));
}

// ---------------------------------------------------------------------------
// Parameters and programs
// ---------------------------------------------------------------------------

int NativeVst2Plugin::parameterCount() const {
    return effect_ ? effect_->numParams : 0;
}

float NativeVst2Plugin::parameterValue(int index) const {
    if (!effect_ || !effect_->getParameter || index < 0 || index >= effect_->numParams) return 0.0f;
    return effect_->getParameter(effect_, index);
}

void NativeVst2Plugin::setParameterValue(int index, float value) {
    if (!effect_ || !effect_->setParameter || index < 0 || index >= effect_->numParams) return;
    effect_->setParameter(effect_, index, clampValue(value, 0.0f, 1.0f));
}

std::string NativeVst2Plugin::parameterName(int index) const {
    const std::string name = dispatchString(effGetParamName, index);
    return name.empty() ? ("Param " + std::to_string(index + 1)) : name;
}

std::string NativeVst2Plugin::parameterLabel(int index) const {
    return dispatchString(effGetParamLabel, index);
}

std::string NativeVst2Plugin::parameterDisplay(int index) const {
    return dispatchString(effGetParamDisplay, index);
}

int NativeVst2Plugin::programCount() const {
    return effect_ ? effect_->numPrograms : 0;
}

int NativeVst2Plugin::currentProgram() const {
    return static_cast<int>(dispatch(effGetProgram));
}

void NativeVst2Plugin::setCurrentProgram(int index) {
    if (!effect_ || index < 0 || index >= effect_->numPrograms) return;
    dispatch(effBeginSetProgram);
    dispatch(effSetProgram, 0, index);
    dispatch(effEndSetProgram);
    parametersChanged_.store(true, std::memory_order_release);
}

std::string NativeVst2Plugin::programName(int index) const {
    char buffer[kStringBufferSize];
    std::memset(buffer, 0, sizeof(buffer));
    if (dispatch(effGetProgramNameIndexed, index, 0, buffer) != 0)
        return fromVstString(buffer, sizeof(buffer));
    return "Program " + std::to_string(index + 1);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> NativeVst2Plugin::saveState() const {
    std::vector<std::uint8_t> out;
    if (!effect_) return out;

    if (description_.usesChunks) {
        void* chunk = nullptr;
        // index 0 asks for the whole bank rather than the current program, which
        // is what a patch should carry.
        const auto size = static_cast<std::size_t>(dispatch(effGetChunk, 0, 0, &chunk));
        if (chunk != nullptr && size > 0 && size < (256u << 20)) {
            out.resize(size);
            std::memcpy(out.data(), chunk, size);
        }
        return out;
    }

    // No chunk support: fall back to a straight dump of the normalised values.
    const int count = effect_->numParams;
    out.resize(static_cast<std::size_t>(count) * sizeof(float));
    for (int i = 0; i < count; ++i) {
        const float value = parameterValue(i);
        std::memcpy(out.data() + static_cast<std::size_t>(i) * sizeof(float), &value, sizeof(float));
    }
    return out;
}

bool NativeVst2Plugin::restoreState(const std::vector<std::uint8_t>& data) {
    if (!effect_ || data.empty()) return false;

    if (description_.usesChunks) {
        dispatch(effSetChunk, 0, static_cast<VstIntPtr>(data.size()),
                 const_cast<std::uint8_t*>(data.data()));
        parametersChanged_.store(true, std::memory_order_release);
        return true;
    }

    const std::size_t count = std::min(static_cast<std::size_t>(effect_->numParams),
                                       data.size() / sizeof(float));
    for (std::size_t i = 0; i < count; ++i) {
        float value = 0.0f;
        std::memcpy(&value, data.data() + i * sizeof(float), sizeof(float));
        setParameterValue(static_cast<int>(i), value);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

bool NativeVst2Plugin::hasEditor() const {
    return effect_ && (effect_->flags & effFlagsHasEditor) != 0;
}

bool NativeVst2Plugin::openEditor() {
    if (!hasEditor() || editorOpen_) return editorOpen_;

    // Ask for the size first so the window is right before the plugin draws into
    // it; some plugins take the window they are given and never resize.
    int width = 640, height = 480;
    struct EditorRect { std::int16_t top, left, bottom, right; };
    EditorRect* rect = nullptr;
    if (dispatch(effEditGetRect, 0, 0, &rect) != 0 && rect != nullptr) {
        const int w = rect->right - rect->left;
        const int h = rect->bottom - rect->top;
        if (w > 0 && h > 0 && w < 8192 && h < 8192) { width = w; height = h; }
    }

    if (!editorWindow_.create(description_.name, width, height)) {
        error_ = "could not create a window for the plugin editor";
        return false;
    }

    if (dispatch(effEditOpen, 0, 0, editorWindow_.nativeHandle()) == 0) {
        // Some plugins return 0 yet open successfully, so this is not fatal on
        // its own; only bail if the plugin also refuses to report a size.
        if (rect == nullptr)
            error_ = "the plugin declined to open its editor";
    }

    // Re-query: plugins commonly report their real size only after effEditOpen.
    if (dispatch(effEditGetRect, 0, 0, &rect) != 0 && rect != nullptr) {
        const int w = rect->right - rect->left;
        const int h = rect->bottom - rect->top;
        if (w > 0 && h > 0 && w < 8192 && h < 8192) editorWindow_.resizeClient(w, h);
    }

    editorOpen_ = true;
    return true;
}

void NativeVst2Plugin::closeEditor() {
    if (!editorOpen_) return;
    dispatch(effEditClose);
    editorWindow_.destroy();
    editorOpen_ = false;
}

void NativeVst2Plugin::idle() {
    if (editorOpen_) dispatch(effEditIdle);

    const int width = editorWidth_.exchange(0, std::memory_order_relaxed);
    const int height = editorHeight_.exchange(0, std::memory_order_relaxed);
    if (width > 0 && height > 0 && editorOpen_)
        editorWindow_.resizeClient(width, height);
}

// ---------------------------------------------------------------------------
// MIDI
// ---------------------------------------------------------------------------

void NativeVst2Plugin::sendMidi(const MidiMessage& message) {
    std::lock_guard<std::mutex> lock(midiMutex_);
    if (pendingMidi_.size() < kMidiCapacity)
        pendingMidi_.push_back(message);
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

bool NativeVst2Plugin::describe(const std::string& utf8Path, PluginDescription& out,
                                std::string* error) {
    NativeVst2Plugin plugin;
    if (!plugin.load(utf8Path, 48000.0, 512)) {
        if (error) *error = plugin.errorText();
        return false;
    }

    out = plugin.description();
    out.fileSize = fileSize(utf8Path);
    return true;
}

} // namespace acm::vst2
