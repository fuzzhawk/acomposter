// acomposter plugin bridge helper.
//
// Built twice from these same sources:
//
//   acomposter-bridge32.exe  - x86, hosts 32-bit VST2 plugins for the 64-bit app
//   acomposter-bridge64.exe  - x64, used for scanning and for optional crash
//                              isolation of 64-bit plugins
//
// It maps the shared block the host created, then sits in a loop waiting for a
// command. The wait is a MsgWaitForMultipleObjects rather than a plain
// WaitForSingleObject, because this process also owns the plugin's editor window
// and that window needs its messages pumped while nothing is being asked of it.
//
// Everything that can go wrong here - a plugin that will not load, one that
// crashes on its first block, one that hangs - is contained: the host notices a
// dead or silent helper and carries on making sound.

#include "../core/Json.h"
#include "../core/Types.h"
#include "../core/Utf.h"
#include "../vst2/BridgeProtocol.h"
#include "../vst2/NativeVst2Plugin.h"
#include "../vst2/PluginEditorWindow.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <objbase.h>    // CoInitializeEx, for plugins whose editors use COM
#include <shellapi.h>   // CommandLineToArgvW

namespace acm::vst2::bridge {
namespace {

class BridgeServer {
public:
    bool start(const std::string& instanceKey);
    void run();
    void stop();

private:
    void handleCommand(Command command);

    void respondOk() { control_->status = static_cast<std::uint32_t>(Status::Ok); }
    void respondError(const std::string& message);
    void respondUnsupported() { control_->status = static_cast<std::uint32_t>(Status::Unsupported); }

    void writeData(const void* data, std::size_t size);
    void writeString(const std::string& text) { writeData(text.data(), text.size()); }
    std::string readString() const;

    void doLoad();
    void doDescribe();
    void doProcess();
    void applyPendingParameters();

    ControlBlock* control_ = nullptr;
    std::uint8_t* base_ = nullptr;
    HANDLE mapping_ = nullptr;
    HANDLE requestEvent_ = nullptr;
    HANDLE responseEvent_ = nullptr;

    std::unique_ptr<NativeVst2Plugin> plugin_;
    bool running_ = false;

    // Channel pointer arrays into the shared audio region, rebuilt when the
    // plugin's channel layout is known.
    std::vector<float*> inputPointers_;
    std::vector<float*> outputPointers_;

    float* audioInput(int channel) const {
        return reinterpret_cast<float*>(base_ + kAudioInputOffset)
             + static_cast<std::size_t>(channel) * static_cast<std::size_t>(kMaxBlockSize);
    }
    float* audioOutput(int channel) const {
        return reinterpret_cast<float*>(base_ + kAudioOutputOffset)
             + static_cast<std::size_t>(channel) * static_cast<std::size_t>(kMaxBlockSize);
    }
    std::uint8_t* dataArea() const { return base_ + kDataOffset; }
};

bool BridgeServer::start(const std::string& instanceKey) {
    const std::wstring mappingName = utf8ToWide(std::string(kSharedMemoryPrefix) + instanceKey);
    const std::wstring requestName = utf8ToWide(std::string(kRequestEventPrefix) + instanceKey);
    const std::wstring responseName = utf8ToWide(std::string(kResponseEventPrefix) + instanceKey);

    mapping_ = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
    if (!mapping_) return false;

    base_ = static_cast<std::uint8_t*>(
        ::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kTotalBytes));
    if (!base_) return false;

    control_ = reinterpret_cast<ControlBlock*>(base_);
    if (control_->magic != kMagic || control_->protocolVersion != kProtocolVersion)
        return false;

    requestEvent_ = ::OpenEventW(EVENT_ALL_ACCESS, FALSE, requestName.c_str());
    responseEvent_ = ::OpenEventW(EVENT_ALL_ACCESS, FALSE, responseName.c_str());
    if (!requestEvent_ || !responseEvent_) return false;

    inputPointers_.reserve(kMaxChannels);
    outputPointers_.reserve(kMaxChannels);

    running_ = true;
    return true;
}

void BridgeServer::stop() {
    running_ = false;
    plugin_.reset();

    if (base_) { ::UnmapViewOfFile(base_); base_ = nullptr; }
    if (mapping_) { ::CloseHandle(mapping_); mapping_ = nullptr; }
    if (requestEvent_) { ::CloseHandle(requestEvent_); requestEvent_ = nullptr; }
    if (responseEvent_) { ::CloseHandle(responseEvent_); responseEvent_ = nullptr; }
}

void BridgeServer::respondError(const std::string& message) {
    writeString(message);
    control_->status = static_cast<std::uint32_t>(Status::Error);
}

void BridgeServer::writeData(const void* data, std::size_t size) {
    size = std::min<std::size_t>(size, kDataBytes);
    if (size > 0) std::memcpy(dataArea(), data, size);
    control_->dataSize = static_cast<std::uint32_t>(size);
}

std::string BridgeServer::readString() const {
    const std::uint32_t size = std::min(control_->dataSize, kDataBytes);
    return std::string(reinterpret_cast<const char*>(dataArea()), size);
}

// ---------------------------------------------------------------------------

void BridgeServer::run() {
    while (running_) {
        // Wait for a command, but wake for window messages too: the plugin's
        // editor lives in this process and stops redrawing if its queue is not
        // serviced while we are idle.
        const DWORD result = ::MsgWaitForMultipleObjects(1, &requestEvent_, FALSE,
                                                         INFINITE, QS_ALLINPUT);

        if (result == WAIT_OBJECT_0) {
            const auto command = static_cast<Command>(control_->command);
            handleCommand(command);
            ::SetEvent(responseEvent_);
            continue;
        }

        if (result == WAIT_OBJECT_0 + 1) {
            if (!PluginEditorWindow::pumpMessages()) {
                running_ = false;
            }
            continue;
        }

        // The wait failed, which in practice means the host went away.
        running_ = false;
    }
}

void BridgeServer::handleCommand(Command command) {
    switch (command) {
        case Command::Ping:
            respondOk();
            break;

        case Command::Describe:
            doDescribe();
            break;

        case Command::Load:
            doLoad();
            break;

        case Command::Unload:
            plugin_.reset();
            respondOk();
            break;

        case Command::SetRateAndBlock:
            if (!plugin_) { respondError("no plugin loaded"); break; }
            plugin_->setSampleRateAndBlockSize(control_->doubleArgs[0], control_->intArgs[0]);
            respondOk();
            break;

        case Command::SetActive:
            if (!plugin_) { respondError("no plugin loaded"); break; }
            plugin_->setActive(control_->intArgs[0] != 0);
            respondOk();
            break;

        case Command::Process:
            doProcess();
            break;

        case Command::SetParameter:
            if (!plugin_) { respondError("no plugin loaded"); break; }
            plugin_->setParameterValue(control_->intArgs[0],
                                       static_cast<float>(control_->doubleArgs[0]));
            respondOk();
            break;

        case Command::GetParameter:
            if (!plugin_) { respondError("no plugin loaded"); break; }
            control_->doubleArgs[0] = plugin_->parameterValue(control_->intArgs[0]);
            respondOk();
            break;

        case Command::GetParameterInfo: {
            if (!plugin_) { respondError("no plugin loaded"); break; }
            const int index = control_->intArgs[0];
            JsonValue info = JsonValue::object();
            info.set("name", plugin_->parameterName(index));
            info.set("label", plugin_->parameterLabel(index));
            info.set("display", plugin_->parameterDisplay(index));
            writeString(info.dump(-1));
            respondOk();
            break;
        }

        case Command::SetProgram:
            if (!plugin_) { respondError("no plugin loaded"); break; }
            plugin_->setCurrentProgram(control_->intArgs[0]);
            respondOk();
            break;

        case Command::GetProgram:
            if (!plugin_) { respondError("no plugin loaded"); break; }
            control_->intArgs[0] = plugin_->currentProgram();
            respondOk();
            break;

        case Command::GetProgramName:
            if (!plugin_) { respondError("no plugin loaded"); break; }
            writeString(plugin_->programName(control_->intArgs[0]));
            respondOk();
            break;

        case Command::GetState: {
            if (!plugin_) { respondError("no plugin loaded"); break; }
            const std::vector<std::uint8_t> state = plugin_->saveState();
            if (state.size() > kDataBytes) {
                respondError("the plugin's state is too large to transfer");
                break;
            }
            writeData(state.data(), state.size());
            respondOk();
            break;
        }

        case Command::SetState: {
            if (!plugin_) { respondError("no plugin loaded"); break; }
            const std::uint32_t size = std::min(control_->dataSize, kDataBytes);
            std::vector<std::uint8_t> state(dataArea(), dataArea() + size);
            plugin_->restoreState(state);
            respondOk();
            break;
        }

        case Command::OpenEditor:
            if (!plugin_) { respondError("no plugin loaded"); break; }
            if (!plugin_->openEditor()) {
                respondError(plugin_->errorText().empty() ? "the plugin would not open its editor"
                                                          : plugin_->errorText());
                break;
            }
            respondOk();
            break;

        case Command::CloseEditor:
            if (plugin_) plugin_->closeEditor();
            respondOk();
            break;

        case Command::EditorIdle:
            if (plugin_) plugin_->idle();
            if (plugin_ && plugin_->consumeParameterRefreshFlag())
                control_->notifyFlags |= kNotifyParametersChanged;
            respondOk();
            break;

        case Command::SendMidi: {
            if (!plugin_) { respondError("no plugin loaded"); break; }
            const int count = clampValue(control_->midiCount, 0, kMaxMidiEvents);
            for (int i = 0; i < count; ++i) {
                const MidiEvent& event = control_->midi[i];
                plugin_->sendMidi(MidiMessage{ event.status, event.data1, event.data2,
                                               event.deltaFrames });
            }
            respondOk();
            break;
        }

        case Command::Shutdown:
            plugin_.reset();
            running_ = false;
            respondOk();
            break;

        case Command::None:
        default:
            respondUnsupported();
            break;
    }
}

void BridgeServer::doDescribe() {
    const std::string path = readString();
    if (path.empty()) { respondError("no plugin path was supplied"); return; }

    PluginDescription description;
    std::string error;
    if (!NativeVst2Plugin::describe(path, description, &error)) {
        respondError(error.empty() ? "the plugin could not be loaded" : error);
        return;
    }

    writeString(toJson(description).dump(-1));
    respondOk();
}

void BridgeServer::doLoad() {
    const std::string path = readString();
    if (path.empty()) { respondError("no plugin path was supplied"); return; }

    const double sampleRate = control_->sampleRate > 0.0 ? control_->sampleRate : 48000.0;
    const int blockSize = clampValue(control_->blockSize, 16, kMaxBlockSize);

    plugin_ = std::make_unique<NativeVst2Plugin>();
    if (!plugin_->load(path, sampleRate, blockSize)) {
        const std::string error = plugin_->errorText();
        plugin_.reset();
        respondError(error.empty() ? "the plugin could not be loaded" : error);
        return;
    }

    const PluginDescription& description = plugin_->description();

    inputPointers_.assign(static_cast<std::size_t>(clampValue(description.numInputs, 0, kMaxChannels)), nullptr);
    outputPointers_.assign(static_cast<std::size_t>(clampValue(description.numOutputs, 0, kMaxChannels)), nullptr);
    for (std::size_t c = 0; c < inputPointers_.size(); ++c) inputPointers_[c] = audioInput(static_cast<int>(c));
    for (std::size_t c = 0; c < outputPointers_.size(); ++c) outputPointers_[c] = audioOutput(static_cast<int>(c));

    JsonValue json = toJson(description);
    json.set("latency", plugin_->latencyFrames());
    writeString(json.dump(-1));
    respondOk();
}

void BridgeServer::applyPendingParameters() {
    // The host packs parameter changes into the data area alongside the audio,
    // so a metasurface sweep costs no extra round trips.
    const std::uint32_t size = std::min(control_->dataSize, kDataBytes);
    if (size < sizeof(std::int32_t)) return;

    const std::uint8_t* data = dataArea();
    std::int32_t count = 0;
    std::memcpy(&count, data, sizeof(count));

    const std::uint32_t needed = sizeof(std::int32_t)
                               + static_cast<std::uint32_t>(count) * (sizeof(std::int32_t) + sizeof(float));
    if (count <= 0 || needed > size) return;

    std::uint32_t offset = sizeof(std::int32_t);
    for (std::int32_t i = 0; i < count; ++i) {
        std::int32_t index = 0;
        float value = 0.0f;
        std::memcpy(&index, data + offset, sizeof(index));
        offset += sizeof(index);
        std::memcpy(&value, data + offset, sizeof(value));
        offset += sizeof(value);
        plugin_->setParameterValue(index, value);
    }
}

void BridgeServer::doProcess() {
    if (!plugin_) { respondError("no plugin loaded"); return; }

    const int frames = clampValue(control_->frames, 0, kMaxBlockSize);
    if (frames == 0) { respondOk(); return; }

    applyPendingParameters();

    const int midiCount = clampValue(control_->midiCount, 0, kMaxMidiEvents);
    for (int i = 0; i < midiCount; ++i) {
        const MidiEvent& event = control_->midi[i];
        plugin_->sendMidi(MidiMessage{ event.status, event.data1, event.data2, event.deltaFrames });
    }
    control_->midiCount = 0;

    HostTimeInfo time;
    time.samplePosition = control_->samplePosition;
    time.sampleRate = control_->sampleRate;
    time.ppqPosition = control_->ppqPosition;
    time.tempo = control_->tempo;
    time.barStartPosition = control_->barStartPosition;
    time.cycleStart = control_->cycleStart;
    time.cycleEnd = control_->cycleEnd;
    time.timeSigNumerator = control_->timeSigNumerator;
    time.timeSigDenominator = control_->timeSigDenominator;
    time.playing = (control_->transportFlags & kPlaying) != 0;
    time.recording = (control_->transportFlags & kRecording) != 0;
    time.cycleActive = (control_->transportFlags & kCycleActive) != 0;

    plugin_->process(inputPointers_.empty() ? nullptr : inputPointers_.data(),
                     static_cast<int>(inputPointers_.size()),
                     outputPointers_.empty() ? nullptr : outputPointers_.data(),
                     static_cast<int>(outputPointers_.size()),
                     frames, time);

    if (plugin_->consumeParameterRefreshFlag())
        control_->notifyFlags |= kNotifyParametersChanged;
    if (!plugin_->editorOpen())
        control_->notifyFlags |= kNotifyEditorClosed;

    control_->dataSize = 0;
    respondOk();
}

// ---------------------------------------------------------------------------

std::string instanceKeyFromCommandLine() {
    int argumentCount = 0;
    LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
    if (!arguments) return {};

    std::string key;
    for (int i = 1; i + 1 < argumentCount; ++i) {
        if (std::wcscmp(arguments[i], L"--key") == 0) {
            key = wideToUtf8(arguments[i + 1]);
            break;
        }
    }

    ::LocalFree(arguments);
    return key;
}

} // namespace
} // namespace acm::vst2::bridge

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    using namespace acm::vst2::bridge;

    // The plugin's own COM usage (file dialogs, some editors) needs an
    // apartment, and it has to be initialised on this thread before anything
    // else touches it.
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const std::string key = instanceKeyFromCommandLine();
    if (key.empty()) {
        // Started by hand rather than by acomposter: say so instead of sitting
        // there invisibly doing nothing.
        ::MessageBoxW(nullptr,
                      L"This is acomposter's VST plugin bridge.\n\n"
                      L"It is started automatically by acomposter and is not meant to be "
                      L"run on its own.",
                      L"acomposter plugin bridge",
                      MB_OK | MB_ICONINFORMATION);
        ::CoUninitialize();
        return 1;
    }

    int exitCode = 0;
    {
        BridgeServer server;
        if (!server.start(key)) {
            exitCode = 2;
        } else {
            server.run();
            server.stop();
        }
    }

    ::CoUninitialize();
    return exitCode;
}
