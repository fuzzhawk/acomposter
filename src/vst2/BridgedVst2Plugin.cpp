#include "BridgedVst2Plugin.h"

#include "../core/FileIo.h"
#include "../core/Json.h"
#include "../core/Types.h"
#include "../core/Utf.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

namespace acm::vst2 {
namespace {

std::string g_helperDirectory;

std::string makeInstanceKey() {
    // Unique per instance so several bridged plugins can run side by side, and
    // so a stale helper from a previous crash cannot latch onto our mailbox.
    static std::atomic<std::uint32_t> counter{ 0 };
    const std::uint32_t index = counter.fetch_add(1, std::memory_order_relaxed);

    LARGE_INTEGER ticks{};
    ::QueryPerformanceCounter(&ticks);

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%lu-%llx-%u",
                  static_cast<unsigned long>(::GetCurrentProcessId()),
                  static_cast<unsigned long long>(ticks.QuadPart),
                  index);
    return buffer;
}

std::string hostDirectory() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) return {};
    return pathParent(wideToUtf8(std::wstring_view(buffer, length)));
}

} // namespace

// ---------------------------------------------------------------------------

void BridgedVst2Plugin::setHelperDirectory(std::string directory) {
    g_helperDirectory = std::move(directory);
}

std::string BridgedVst2Plugin::helperPathFor(Architecture architecture) {
    const std::string directory = g_helperDirectory.empty() ? hostDirectory() : g_helperDirectory;
    const char* name = (architecture == Architecture::X86) ? "acomposter-bridge32.exe"
                                                           : "acomposter-bridge64.exe";
    return pathJoin(directory, name);
}

BridgedVst2Plugin::BridgedVst2Plugin() {
    pendingParameters_.reserve(256);
    pendingMidi_.reserve(bridge::kMaxMidiEvents);
}

BridgedVst2Plugin::~BridgedVst2Plugin() {
    stopHelper();
}

std::string BridgedVst2Plugin::errorText() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return error_;
}

void BridgedVst2Plugin::markDead(const std::string& reason) const {
    alive_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(errorMutex_);
    if (error_.empty()) error_ = reason;
}

// ---------------------------------------------------------------------------
// Shared memory access
// ---------------------------------------------------------------------------

bridge::ControlBlock* BridgedVst2Plugin::control() const {
    return static_cast<bridge::ControlBlock*>(view_);
}

float* BridgedVst2Plugin::audioInput(int channel) const {
    auto* base = static_cast<std::uint8_t*>(view_) + bridge::kAudioInputOffset;
    return reinterpret_cast<float*>(base)
         + static_cast<std::size_t>(channel) * static_cast<std::size_t>(bridge::kMaxBlockSize);
}

float* BridgedVst2Plugin::audioOutput(int channel) const {
    auto* base = static_cast<std::uint8_t*>(view_) + bridge::kAudioOutputOffset;
    return reinterpret_cast<float*>(base)
         + static_cast<std::size_t>(channel) * static_cast<std::size_t>(bridge::kMaxBlockSize);
}

std::uint8_t* BridgedVst2Plugin::dataArea() const {
    return static_cast<std::uint8_t*>(view_) + bridge::kDataOffset;
}

void BridgedVst2Plugin::writeData(const void* data, std::size_t size) const {
    if (!view_) return;
    size = std::min<std::size_t>(size, bridge::kDataBytes);
    std::memcpy(dataArea(), data, size);
    control()->dataSize = static_cast<std::uint32_t>(size);
}

std::string BridgedVst2Plugin::readDataAsString() const {
    if (!view_) return {};
    const std::uint32_t size = std::min(control()->dataSize, bridge::kDataBytes);
    return std::string(reinterpret_cast<const char*>(dataArea()), size);
}

std::vector<std::uint8_t> BridgedVst2Plugin::readDataAsBytes() const {
    if (!view_) return {};
    const std::uint32_t size = std::min(control()->dataSize, bridge::kDataBytes);
    return std::vector<std::uint8_t>(dataArea(), dataArea() + size);
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

bool BridgedVst2Plugin::transact(bridge::Command command, std::uint32_t timeoutMs) const {
    if (!view_ || !requestEvent_ || !responseEvent_) return false;
    if (!alive_.load(std::memory_order_relaxed) && command != bridge::Command::Shutdown) return false;

    bridge::ControlBlock* block = control();
    block->command = static_cast<std::uint32_t>(command);
    block->status = static_cast<std::uint32_t>(bridge::Status::Idle);
    block->sequence = ++sequence_;

    ::SetEvent(static_cast<HANDLE>(requestEvent_));

    const DWORD result = ::WaitForSingleObject(static_cast<HANDLE>(responseEvent_), timeoutMs);

    if (result != WAIT_OBJECT_0) {
        // Distinguish "still working" from "gone": a helper that has exited is
        // never coming back, so do not keep waiting on it.
        DWORD exitCode = 0;
        const bool exited = processHandle_ != nullptr
                         && ::GetExitCodeProcess(static_cast<HANDLE>(processHandle_), &exitCode)
                         && exitCode != STILL_ACTIVE;

        if (exited) {
            markDead("the plugin bridge process exited unexpectedly (the plugin most likely crashed)");
            return false;
        }

        if (command == bridge::Command::Process) {
            if (++consecutiveTimeouts_ >= kMaxConsecutiveTimeouts) {
                markDead("the plugin stopped responding and has been disconnected");
                return false;
            }
            // Tolerate the odd late block rather than killing the plugin.
            return false;
        }

        markDead("the plugin bridge timed out");
        return false;
    }

    if (command == bridge::Command::Process) consecutiveTimeouts_ = 0;

    if (block->status == static_cast<std::uint32_t>(bridge::Status::Error)) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_ = readDataAsString();
        if (error_.empty()) error_ = "the plugin bridge reported an error";
        return false;
    }

    return block->status == static_cast<std::uint32_t>(bridge::Status::Ok);
}

// ---------------------------------------------------------------------------
// Helper lifecycle
// ---------------------------------------------------------------------------

bool BridgedVst2Plugin::startHelper(Architecture architecture, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_ = message;
        stopHelper();
        return false;
    };

    instanceKey_ = makeInstanceKey();
    architecture_ = architecture;

    const std::wstring mappingName = utf8ToWide(std::string(bridge::kSharedMemoryPrefix) + instanceKey_);
    const std::wstring requestName = utf8ToWide(std::string(bridge::kRequestEventPrefix) + instanceKey_);
    const std::wstring responseName = utf8ToWide(std::string(bridge::kResponseEventPrefix) + instanceKey_);

    mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, bridge::kTotalBytes, mappingName.c_str());
    if (!mapping_) return fail("could not create the shared memory block for the plugin bridge");

    view_ = ::MapViewOfFile(static_cast<HANDLE>(mapping_), FILE_MAP_ALL_ACCESS, 0, 0, bridge::kTotalBytes);
    if (!view_) return fail("could not map the plugin bridge's shared memory");

    std::memset(view_, 0, bridge::kControlBytes);
    control()->magic = bridge::kMagic;
    control()->protocolVersion = bridge::kProtocolVersion;

    // Auto-reset events: exactly one waiter is released per signal, which is the
    // ping-pong handshake this protocol needs.
    requestEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, requestName.c_str());
    responseEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, responseName.c_str());
    if (!requestEvent_ || !responseEvent_)
        return fail("could not create the plugin bridge's synchronisation objects");

    const std::string helper = helperPathFor(architecture);
    if (!fileExists(helper)) {
        return fail("the plugin bridge helper is missing: " + helper
                    + " (it ships alongside acomposter.exe)");
    }

    std::wstring commandLine = L"\"" + utf8ToWide(helper) + L"\" --key " + utf8ToWide(instanceKey_);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    if (!::CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo)) {
        return fail("could not start the plugin bridge helper (" + std::to_string(::GetLastError()) + ")");
    }

    ::CloseHandle(processInfo.hThread);
    processHandle_ = processInfo.hProcess;
    alive_.store(true, std::memory_order_relaxed);

    // The helper opens the shared objects by name at start-up; give it a moment
    // and confirm with a ping before trusting it.
    if (!transact(bridge::Command::Ping, bridge::kDefaultTimeoutMs))
        return fail("the plugin bridge helper did not respond after starting");

    return true;
}

void BridgedVst2Plugin::stopHelper() {
    if (alive_.load(std::memory_order_relaxed) && view_) {
        transact(bridge::Command::Shutdown, 2000);
    }

    if (processHandle_) {
        // Give it a moment to exit cleanly, then insist.
        if (::WaitForSingleObject(static_cast<HANDLE>(processHandle_), 3000) != WAIT_OBJECT_0)
            ::TerminateProcess(static_cast<HANDLE>(processHandle_), 1);
        ::CloseHandle(static_cast<HANDLE>(processHandle_));
        processHandle_ = nullptr;
    }

    if (view_) { ::UnmapViewOfFile(view_); view_ = nullptr; }
    if (mapping_) { ::CloseHandle(static_cast<HANDLE>(mapping_)); mapping_ = nullptr; }
    if (requestEvent_) { ::CloseHandle(static_cast<HANDLE>(requestEvent_)); requestEvent_ = nullptr; }
    if (responseEvent_) { ::CloseHandle(static_cast<HANDLE>(responseEvent_)); responseEvent_ = nullptr; }

    alive_.store(false, std::memory_order_relaxed);
    loaded_ = false;
    editorOpen_ = false;
}

bool BridgedVst2Plugin::load(const std::string& utf8Path, Architecture architecture,
                             double sampleRate, int blockSize) {
    stopHelper();
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_.clear();
    }

    path_ = utf8Path;
    sampleRate_ = sampleRate;
    blockSize_ = clampValue(blockSize, 16, bridge::kMaxBlockSize);

    std::string startError;
    if (!startHelper(architecture, &startError)) return false;

    writeData(utf8Path.data(), utf8Path.size());
    control()->sampleRate = sampleRate_;
    control()->blockSize = blockSize_;

    if (!transact(bridge::Command::Load, bridge::kLoadTimeoutMs)) {
        stopHelper();
        return false;
    }

    std::string parseError;
    const JsonValue json = JsonValue::parse(readDataAsString(), &parseError);
    if (!parseError.empty()) {
        markDead("the plugin bridge returned a description we could not read");
        stopHelper();
        return false;
    }

    description_ = pluginDescriptionFromJson(json);
    description_.path = utf8Path;
    description_.architecture = architecture;
    latency_ = json.getInt("latency", 0);

    parameterCache_.assign(static_cast<std::size_t>(std::max(0, description_.numParameters)), 0.0f);
    refreshParameterCache();

    loaded_ = true;
    return true;
}

bool BridgedVst2Plugin::relaunch() {
    // Preserve the plugin's state across the restart where we still can.
    std::vector<std::uint8_t> state;
    if (alive_.load(std::memory_order_relaxed)) state = saveState();

    const std::string path = path_;
    const Architecture architecture = architecture_;
    const bool wasActive = active_;

    stopHelper();
    if (!load(path, architecture, sampleRate_, blockSize_)) return false;

    if (!state.empty()) restoreState(state);
    if (wasActive) setActive(true);
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void BridgedVst2Plugin::setSampleRateAndBlockSize(double sampleRate, int blockSize) {
    if (!valid()) return;

    sampleRate_ = sampleRate;
    blockSize_ = clampValue(blockSize, 16, bridge::kMaxBlockSize);

    control()->sampleRate = sampleRate_;
    control()->intArgs[0] = blockSize_;
    control()->doubleArgs[0] = sampleRate_;
    transact(bridge::Command::SetRateAndBlock, bridge::kDefaultTimeoutMs);
}

void BridgedVst2Plugin::setActive(bool active) {
    if (!valid() || active == active_) return;

    control()->intArgs[0] = active ? 1 : 0;
    if (transact(bridge::Command::SetActive, bridge::kDefaultTimeoutMs))
        active_ = active;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void BridgedVst2Plugin::process(const float* const* inputs, int numInputs,
                                float* const* outputs, int numOutputs,
                                int frames, const HostTimeInfo& time) {
    const auto silenceOutputs = [&] {
        for (int c = 0; c < numOutputs; ++c)
            std::memset(outputs[c], 0, sizeof(float) * static_cast<std::size_t>(frames));
    };

    if (!valid() || !active_ || frames <= 0 || frames > bridge::kMaxBlockSize) {
        silenceOutputs();
        return;
    }

    bridge::ControlBlock* block = control();

    // Parameter changes ride along with the block rather than costing an IPC
    // round trip each. The data area carries a count followed by index/value
    // pairs, which the helper applies before it processes.
    {
        std::uint32_t offset = 0;
        auto* data = dataArea();
        const auto count = static_cast<std::int32_t>(pendingParameters_.size());
        std::memcpy(data, &count, sizeof(count));
        offset += sizeof(count);

        for (const PendingParameter& change : pendingParameters_) {
            std::memcpy(data + offset, &change.index, sizeof(change.index));
            offset += sizeof(change.index);
            std::memcpy(data + offset, &change.value, sizeof(change.value));
            offset += sizeof(change.value);
        }
        block->dataSize = offset;
        pendingParameters_.clear();
    }

    {
        std::unique_lock<std::mutex> lock(midiMutex_, std::try_to_lock);
        if (lock.owns_lock() && !pendingMidi_.empty()) {
            const auto count = std::min<std::size_t>(pendingMidi_.size(), bridge::kMaxMidiEvents);
            std::memcpy(block->midi, pendingMidi_.data(), count * sizeof(bridge::MidiEvent));
            block->midiCount = static_cast<std::int32_t>(count);
            pendingMidi_.clear();
        } else {
            block->midiCount = 0;
        }
    }

    const int channelsIn = std::min({ numInputs, description_.numInputs, static_cast<int>(bridge::kMaxChannels) });
    const int channelsOut = std::min({ numOutputs, description_.numOutputs, static_cast<int>(bridge::kMaxChannels) });

    for (int c = 0; c < channelsIn; ++c)
        std::memcpy(audioInput(c), inputs[c], sizeof(float) * static_cast<std::size_t>(frames));

    block->frames = frames;
    block->pluginInputs = description_.numInputs;
    block->pluginOutputs = description_.numOutputs;
    block->samplePosition = time.samplePosition;
    block->sampleRate = time.sampleRate;
    block->ppqPosition = time.ppqPosition;
    block->tempo = time.tempo;
    block->barStartPosition = time.barStartPosition;
    block->cycleStart = time.cycleStart;
    block->cycleEnd = time.cycleEnd;
    block->timeSigNumerator = time.timeSigNumerator;
    block->timeSigDenominator = time.timeSigDenominator;
    block->transportFlags = (time.playing ? bridge::kPlaying : 0u)
                          | (time.recording ? bridge::kRecording : 0u)
                          | (time.cycleActive ? bridge::kCycleActive : 0u);

    if (!transact(bridge::Command::Process, bridge::kProcessTimeoutMs)) {
        silenceOutputs();
        return;
    }

    for (int c = 0; c < channelsOut; ++c)
        std::memcpy(outputs[c], audioOutput(c), sizeof(float) * static_cast<std::size_t>(frames));

    // Channels the plugin does not produce must not carry stale audio.
    for (int c = channelsOut; c < numOutputs; ++c)
        std::memset(outputs[c], 0, sizeof(float) * static_cast<std::size_t>(frames));

    if (block->notifyFlags & bridge::kNotifyParametersChanged) {
        parametersChanged_.store(true, std::memory_order_release);
        block->notifyFlags &= ~bridge::kNotifyParametersChanged;
    }
    if (block->notifyFlags & bridge::kNotifyEditorClosed) {
        editorOpen_ = false;
        block->notifyFlags &= ~bridge::kNotifyEditorClosed;
    }
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

float BridgedVst2Plugin::parameterValue(int index) const {
    if (index < 0 || index >= static_cast<int>(parameterCache_.size())) return 0.0f;
    return parameterCache_[static_cast<std::size_t>(index)];
}

void BridgedVst2Plugin::setParameterValue(int index, float value) {
    if (index < 0 || index >= static_cast<int>(parameterCache_.size())) return;

    value = clampValue(value, 0.0f, 1.0f);
    parameterCache_[static_cast<std::size_t>(index)] = value;

    // Bounded: a runaway modulation source must not grow this without limit.
    if (pendingParameters_.size() < 4096)
        pendingParameters_.push_back(PendingParameter{ index, value });
}

void BridgedVst2Plugin::refreshParameterCache() {
    if (!valid()) return;

    for (std::size_t i = 0; i < parameterCache_.size(); ++i) {
        control()->intArgs[0] = static_cast<std::int32_t>(i);
        if (transact(bridge::Command::GetParameter, bridge::kDefaultTimeoutMs))
            parameterCache_[i] = static_cast<float>(control()->doubleArgs[0]);
    }
}

std::string BridgedVst2Plugin::parameterName(int index) const {
    if (!valid() || index < 0) return "Param " + std::to_string(index + 1);

    control()->intArgs[0] = index;
    if (!transact(bridge::Command::GetParameterInfo, bridge::kDefaultTimeoutMs))
        return "Param " + std::to_string(index + 1);

    const JsonValue info = JsonValue::parse(readDataAsString());
    const std::string name = info.getString("name");
    return name.empty() ? ("Param " + std::to_string(index + 1)) : name;
}

std::string BridgedVst2Plugin::parameterLabel(int index) const {
    if (!valid() || index < 0) return {};
    control()->intArgs[0] = index;
    if (!transact(bridge::Command::GetParameterInfo, bridge::kDefaultTimeoutMs)) return {};
    return JsonValue::parse(readDataAsString()).getString("label");
}

std::string BridgedVst2Plugin::parameterDisplay(int index) const {
    if (!valid() || index < 0) return {};
    control()->intArgs[0] = index;
    if (!transact(bridge::Command::GetParameterInfo, bridge::kDefaultTimeoutMs)) return {};
    return JsonValue::parse(readDataAsString()).getString("display");
}

// ---------------------------------------------------------------------------
// Programs
// ---------------------------------------------------------------------------

int BridgedVst2Plugin::currentProgram() const {
    if (!valid()) return 0;
    if (!transact(bridge::Command::GetProgram, bridge::kDefaultTimeoutMs)) return 0;
    return control()->intArgs[0];
}

void BridgedVst2Plugin::setCurrentProgram(int index) {
    if (!valid()) return;
    control()->intArgs[0] = index;
    if (transact(bridge::Command::SetProgram, bridge::kDefaultTimeoutMs)) {
        refreshParameterCache();
        parametersChanged_.store(true, std::memory_order_release);
    }
}

std::string BridgedVst2Plugin::programName(int index) const {
    if (!valid()) return "Program " + std::to_string(index + 1);
    control()->intArgs[0] = index;
    if (!transact(bridge::Command::GetProgramName, bridge::kDefaultTimeoutMs))
        return "Program " + std::to_string(index + 1);
    const std::string name = readDataAsString();
    return name.empty() ? ("Program " + std::to_string(index + 1)) : name;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> BridgedVst2Plugin::saveState() const {
    if (!valid()) return {};
    if (!transact(bridge::Command::GetState, bridge::kDefaultTimeoutMs)) return {};
    return readDataAsBytes();
}

bool BridgedVst2Plugin::restoreState(const std::vector<std::uint8_t>& data) {
    if (!valid() || data.empty()) return false;
    if (data.size() > bridge::kDataBytes) {
        markDead("the plugin's saved state is too large for the bridge");
        return false;
    }

    writeData(data.data(), data.size());
    if (!transact(bridge::Command::SetState, bridge::kLoadTimeoutMs)) return false;

    refreshParameterCache();
    parametersChanged_.store(true, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

bool BridgedVst2Plugin::openEditor() {
    if (!valid() || !description_.hasEditor) return false;
    if (editorOpen_) return true;

    if (!transact(bridge::Command::OpenEditor, bridge::kDefaultTimeoutMs)) return false;
    editorOpen_ = true;
    return true;
}

void BridgedVst2Plugin::closeEditor() {
    if (!valid() || !editorOpen_) return;
    transact(bridge::Command::CloseEditor, bridge::kDefaultTimeoutMs);
    editorOpen_ = false;
}

void BridgedVst2Plugin::idle() {
    if (!valid()) return;

    // The helper pumps its own message loop while it waits, so this is only
    // needed to give plugins their effEditIdle tick.
    if (editorOpen_)
        transact(bridge::Command::EditorIdle, bridge::kDefaultTimeoutMs);

    if (control()->notifyFlags & bridge::kNotifyParametersChanged) {
        control()->notifyFlags &= ~bridge::kNotifyParametersChanged;
        refreshParameterCache();
        parametersChanged_.store(true, std::memory_order_release);
    }
    if (control()->notifyFlags & bridge::kNotifyEditorClosed) {
        control()->notifyFlags &= ~bridge::kNotifyEditorClosed;
        editorOpen_ = false;
    }
}

// ---------------------------------------------------------------------------
// MIDI
// ---------------------------------------------------------------------------

void BridgedVst2Plugin::sendMidi(const MidiMessage& message) {
    std::lock_guard<std::mutex> lock(midiMutex_);
    if (pendingMidi_.size() >= bridge::kMaxMidiEvents) return;

    bridge::MidiEvent event{};
    event.status = message.status;
    event.data1 = message.data1;
    event.data2 = message.data2;
    event.deltaFrames = message.deltaFrames;
    pendingMidi_.push_back(event);
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

bool BridgedVst2Plugin::describe(const std::string& utf8Path, Architecture architecture,
                                 PluginDescription& out, std::string* error) {
    BridgedVst2Plugin bridgeInstance;

    std::string startError;
    if (!bridgeInstance.startHelper(architecture, &startError)) {
        if (error) *error = startError;
        return false;
    }

    bridgeInstance.writeData(utf8Path.data(), utf8Path.size());

    if (!bridgeInstance.transact(bridge::Command::Describe, bridge::kLoadTimeoutMs)) {
        if (error) *error = bridgeInstance.errorText().empty()
                                ? "the plugin could not be scanned"
                                : bridgeInstance.errorText();
        return false;
    }

    std::string parseError;
    const JsonValue json = JsonValue::parse(bridgeInstance.readDataAsString(), &parseError);
    if (!parseError.empty()) {
        if (error) *error = "the scan returned an unreadable description";
        return false;
    }

    out = pluginDescriptionFromJson(json);
    out.path = utf8Path;
    out.architecture = architecture;
    return true;
}

} // namespace acm::vst2
