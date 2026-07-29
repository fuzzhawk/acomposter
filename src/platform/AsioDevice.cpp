#include "AsioDevice.h"

#include "AsioAbi.h"
#include "../core/Types.h"
#include "../core/Utf.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include <windows.h>
#include <objbase.h>

namespace acm::platform {
namespace {

using namespace acm::platform::asio;

constexpr const wchar_t* kAsioRegistryRoot = L"SOFTWARE\\ASIO";

// A 64-bit host wants the 64-bit registry view explicitly. Under WOW64 the
// default view is the 32-bit one, and a 64-bit driver would be invisible.
constexpr REGSAM kRegistryView = KEY_READ | KEY_WOW64_64KEY;

std::string readRegistryString(HKEY key, const wchar_t* valueName) {
    wchar_t buffer[512];
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (::RegQueryValueExW(key, valueName, nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};

    const std::size_t characters = size / sizeof(wchar_t);
    std::wstring_view view(buffer, characters);
    while (!view.empty() && view.back() == L'\0') view.remove_suffix(1);
    return wideToUtf8(view);
}

// A driver's four-byte-aligned name, trimmed of whatever padding it left.
std::string trimmed(const char* text, std::size_t capacity) {
    std::size_t length = 0;
    while (length < capacity && text[length] != '\0') ++length;
    while (length > 0 && static_cast<unsigned char>(text[length - 1]) <= ' ') --length;
    return std::string(text, length);
}

struct DriverEntry {
    std::string name;      // the Description value, or the key name
    std::string clsidText; // "{...}"
    CLSID clsid{};
};

std::vector<DriverEntry> enumerateDrivers() {
    std::vector<DriverEntry> drivers;

    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kAsioRegistryRoot, 0, kRegistryView, &root)
        != ERROR_SUCCESS)
        return drivers;

    for (DWORD index = 0;; ++index) {
        wchar_t keyName[256];
        DWORD keyLength = static_cast<DWORD>(std::size(keyName));
        if (::RegEnumKeyExW(root, index, keyName, &keyLength, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;

        HKEY driverKey = nullptr;
        if (::RegOpenKeyExW(root, keyName, 0, kRegistryView, &driverKey) != ERROR_SUCCESS)
            continue;

        DriverEntry entry;
        entry.clsidText = readRegistryString(driverKey, L"CLSID");
        entry.name = readRegistryString(driverKey, L"Description");
        ::RegCloseKey(driverKey);

        if (entry.name.empty()) entry.name = wideToUtf8(std::wstring_view(keyName, keyLength));

        // A key with no usable CLSID is a leftover from an uninstall. Skipping
        // it here is the difference between a tidy list and one full of drivers
        // that fail the moment they are chosen.
        if (entry.clsidText.empty()) continue;
        if (::CLSIDFromString(utf8ToWide(entry.clsidText).c_str(), &entry.clsid) != NOERROR)
            continue;

        drivers.push_back(std::move(entry));
    }

    ::RegCloseKey(root);
    return drivers;
}

// Instantiates a driver. The CLSID is passed as the interface id too, which is
// how ASIO has always worked: there is no separate IID for IAsio.
IAsio* createDriver(const CLSID& clsid) {
    void* raw = nullptr;
    if (::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &raw) != S_OK)
        return nullptr;
    return static_cast<IAsio*>(raw);
}

// -- sample conversion ------------------------------------------------------

bool isSupportedSampleType(long type) {
    switch (type) {
        case kInt16LSB:
        case kInt24LSB:
        case kInt32LSB:
        case kFloat32LSB:
        case kFloat64LSB:
        case kInt32LSB16:
        case kInt32LSB18:
        case kInt32LSB20:
        case kInt32LSB24:
            return true;
        default:
            // The big-endian and DSD types exist for hardware that does not ship
            // a Windows driver, so refusing them costs nothing and refusing them
            // loudly beats writing noise into someone's monitors.
            return false;
    }
}

const char* sampleTypeName(long type) {
    switch (type) {
        case kInt16LSB:   return "int16";
        case kInt24LSB:   return "int24";
        case kInt32LSB:   return "int32";
        case kFloat32LSB: return "float32";
        case kFloat64LSB: return "float64";
        case kInt32LSB16: return "int32/16";
        case kInt32LSB18: return "int32/18";
        case kInt32LSB20: return "int32/20";
        case kInt32LSB24: return "int32/24";
        default:          return "unsupported";
    }
}

// How far to shift a right-aligned sample to sit in the top of a 32-bit word.
int int32ShiftFor(long type) {
    switch (type) {
        case kInt32LSB16: return 16;
        case kInt32LSB18: return 14;
        case kInt32LSB20: return 12;
        case kInt32LSB24: return 8;
        default:          return 0;
    }
}

// Writes `frames` samples of one channel out of an interleaved float buffer.
void writeChannel(void* destination, long type, const float* source, int sourceStride,
                  int frames) {
    switch (type) {
        case kFloat32LSB: {
            auto* out = static_cast<float*>(destination);
            for (int i = 0; i < frames; ++i) out[i] = source[i * sourceStride];
            break;
        }

        case kFloat64LSB: {
            auto* out = static_cast<double*>(destination);
            for (int i = 0; i < frames; ++i) out[i] = static_cast<double>(source[i * sourceStride]);
            break;
        }

        case kInt16LSB: {
            auto* out = static_cast<std::int16_t*>(destination);
            for (int i = 0; i < frames; ++i) {
                const float value = clampValue(source[i * sourceStride], -1.0f, 1.0f);
                out[i] = static_cast<std::int16_t>(std::lrintf(value * 32767.0f));
            }
            break;
        }

        case kInt24LSB: {
            // Three bytes per sample, little-endian, no padding.
            auto* out = static_cast<std::uint8_t*>(destination);
            for (int i = 0; i < frames; ++i) {
                const float value = clampValue(source[i * sourceStride], -1.0f, 1.0f);
                const std::int32_t sample = static_cast<std::int32_t>(std::lrintf(value * 8388607.0f));
                out[i * 3 + 0] = static_cast<std::uint8_t>(sample & 0xff);
                out[i * 3 + 1] = static_cast<std::uint8_t>((sample >> 8) & 0xff);
                out[i * 3 + 2] = static_cast<std::uint8_t>((sample >> 16) & 0xff);
            }
            break;
        }

        default: {
            // The whole int32 family, including the right-aligned variants.
            const int shift = int32ShiftFor(type);
            auto* out = static_cast<std::int32_t*>(destination);
            for (int i = 0; i < frames; ++i) {
                const float value = clampValue(source[i * sourceStride], -1.0f, 1.0f);
                const double scaled = static_cast<double>(value) * 2147483647.0;
                out[i] = static_cast<std::int32_t>(std::llrint(scaled)) >> shift;
            }
            break;
        }
    }
}

// Reads one channel into an interleaved float buffer.
void readChannel(const void* source, long type, float* destination, int destinationStride,
                 int frames) {
    switch (type) {
        case kFloat32LSB: {
            const auto* in = static_cast<const float*>(source);
            for (int i = 0; i < frames; ++i) destination[i * destinationStride] = in[i];
            break;
        }

        case kFloat64LSB: {
            const auto* in = static_cast<const double*>(source);
            for (int i = 0; i < frames; ++i)
                destination[i * destinationStride] = static_cast<float>(in[i]);
            break;
        }

        case kInt16LSB: {
            const auto* in = static_cast<const std::int16_t*>(source);
            for (int i = 0; i < frames; ++i)
                destination[i * destinationStride] = static_cast<float>(in[i]) / 32768.0f;
            break;
        }

        case kInt24LSB: {
            const auto* in = static_cast<const std::uint8_t*>(source);
            for (int i = 0; i < frames; ++i) {
                std::int32_t sample = static_cast<std::int32_t>(in[i * 3 + 0])
                                    | (static_cast<std::int32_t>(in[i * 3 + 1]) << 8)
                                    | (static_cast<std::int32_t>(in[i * 3 + 2]) << 16);
                if (sample & 0x800000) sample |= ~0xffffff;   // sign extend
                destination[i * destinationStride] = static_cast<float>(sample) / 8388608.0f;
            }
            break;
        }

        default: {
            const int shift = int32ShiftFor(type);
            const auto* in = static_cast<const std::int32_t*>(source);
            for (int i = 0; i < frames; ++i) {
                const std::int32_t sample = in[i] << shift;
                destination[i * destinationStride] = static_cast<float>(
                    static_cast<double>(sample) / 2147483648.0);
            }
            break;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct AsioDevice::Impl {
    IAsio* driver = nullptr;
    bool driverInitialised = false;
    bool buffersCreated = false;
    bool started = false;
    bool supportsOutputReady = false;

    AudioDevice::Callback callback;
    AudioDeviceStatus status;
    std::string driverName;

    long bufferFrames = 0;

    // Everything the callback touches, laid out so the switch does no work it
    // does not have to. The vectors are sized once in open() and never resized
    // while the driver is running.
    std::vector<AsioBufferInfo> bufferInfos;
    std::vector<long> outputTypes;
    std::vector<long> inputTypes;

    int deviceOutputChannels = 0;
    int deviceInputChannels = 0;
    int busChannels = 2;
    int channelOffset = 0;
    int inputChannels = 0;

    std::vector<float> busScratch;     // interleaved, what the engine renders into
    std::vector<float> inputScratch;   // interleaved, what the engine reads

    std::atomic<bool> running{ false };
    // Raised by the driver from its own thread when it wants a reset. Acted on
    // by close/reopen from the message thread; doing it inside the callback
    // would be re-entering the driver from its own callback.
    std::atomic<bool> resetRequested{ false };

    void switchBuffers(long bufferIndex);
};

// ---------------------------------------------------------------------------
// The process-wide active driver
//
// ASIO callbacks have no user pointer, so this is the only way back to the
// object. Guarded because open() and close() run on the message thread while
// the callbacks run on the driver's.
// ---------------------------------------------------------------------------

namespace {

std::mutex g_activeMutex;
AsioDevice::Impl* g_active = nullptr;

AsioDevice::Impl* activeImpl() {
    // No lock in the callback path: the pointer is a single aligned word, it is
    // only ever set before start() and cleared after stop(), and the driver
    // guarantees no callback is in flight across either.
    return g_active;
}

void callbackBufferSwitch(long bufferIndex, AsioBool /*directProcess*/) {
    if (AsioDevice::Impl* impl = activeImpl()) impl->switchBuffers(bufferIndex);
}

void callbackSampleRateDidChange(AsioSampleRate /*rate*/) {
    // The graph is prepared for one rate. Rather than reconfigure underneath the
    // audio thread, ask for the same reset path a driver reset takes.
    if (AsioDevice::Impl* impl = activeImpl())
        impl->resetRequested.store(true, std::memory_order_release);
}

long callbackAsioMessage(long selector, long value, void* /*message*/, double* /*opt*/) {
    switch (selector) {
        case kSelectorSupported:
            switch (value) {
                case kResetRequest:
                case kResyncRequest:
                case kLatenciesChanged:
                case kEngineVersion:
                case kSupportsTimeInfo:
                    return 1;
                default:
                    return 0;
            }

        case kEngineVersion:
            return 2;

        case kResetRequest:
        case kResyncRequest:
            if (AsioDevice::Impl* impl = activeImpl())
                impl->resetRequested.store(true, std::memory_order_release);
            return 1;

        case kLatenciesChanged:
            return 1;

        // Answered yes so drivers that prefer bufferSwitchTimeInfo will use it;
        // the timing fields are ignored, because the engine's clock comes from
        // the frame count and not from the driver's sample position.
        case kSupportsTimeInfo:
            return 1;

        default:
            return 0;
    }
}

AsioTime* callbackBufferSwitchTimeInfo(AsioTime* params, long bufferIndex,
                                       AsioBool /*directProcess*/) {
    if (AsioDevice::Impl* impl = activeImpl()) impl->switchBuffers(bufferIndex);
    return params;
}

AsioCallbacks g_callbacks{
    &callbackBufferSwitch,
    &callbackSampleRateDidChange,
    &callbackAsioMessage,
    &callbackBufferSwitchTimeInfo,
};

} // namespace

// ---------------------------------------------------------------------------
// The audio callback
// ---------------------------------------------------------------------------

void AsioDevice::Impl::switchBuffers(long bufferIndex) {
    const int frames = static_cast<int>(bufferFrames);
    if (frames <= 0) return;

    const std::size_t half = static_cast<std::size_t>(bufferIndex & 1);

    // -- gather the input ---------------------------------------------------
    const float* inputPointer = nullptr;
    if (inputChannels > 0) {
        std::memset(inputScratch.data(), 0,
                    sizeof(float) * static_cast<std::size_t>(frames) * inputChannels);

        for (int c = 0; c < inputChannels; ++c) {
            const AsioBufferInfo& info = bufferInfos[static_cast<std::size_t>(c)];
            if (info.buffers[half] == nullptr) continue;
            readChannel(info.buffers[half], inputTypes[static_cast<std::size_t>(c)],
                        inputScratch.data() + c, inputChannels, frames);
        }
        inputPointer = inputScratch.data();
    }

    // -- render -------------------------------------------------------------
    if (callback) {
        callback(inputPointer, inputChannels, busScratch.data(), busChannels, frames);
    } else {
        std::memset(busScratch.data(), 0,
                    sizeof(float) * static_cast<std::size_t>(frames) * busChannels);
    }

    // -- scatter onto the device's channels ---------------------------------
    // Outputs come after inputs in bufferInfos, in the order they were asked
    // for. Channels the patch does not reach are silenced rather than left as
    // they were: whatever the driver had in that half of its double buffer is
    // the previous block, and repeating it is an audible buzz.
    const std::size_t outputBase = static_cast<std::size_t>(inputChannels);
    for (int c = 0; c < deviceOutputChannels; ++c) {
        const AsioBufferInfo& info = bufferInfos[outputBase + static_cast<std::size_t>(c)];
        void* destination = info.buffers[half];
        if (destination == nullptr) continue;

        const long type = outputTypes[static_cast<std::size_t>(c)];
        const int busChannel = c - channelOffset;

        if (busChannel < 0 || busChannel >= busChannels) {
            const std::size_t bytesPerSample =
                type == kInt16LSB ? 2u : type == kInt24LSB ? 3u : type == kFloat64LSB ? 8u : 4u;
            std::memset(destination, 0, bytesPerSample * static_cast<std::size_t>(frames));
            continue;
        }

        writeChannel(destination, type, busScratch.data() + busChannel, busChannels, frames);
    }

    // Drivers that report they need it want to be told the buffer is filled.
    if (supportsOutputReady && driver) driver->outputReady();
}

// ---------------------------------------------------------------------------
// AsioDevice
// ---------------------------------------------------------------------------

AsioDevice::AsioDevice() : impl_(std::make_unique<Impl>()) {}

AsioDevice::~AsioDevice() { close(); }

std::vector<AudioDeviceInfo> AsioDevice::drivers() {
    std::vector<AudioDeviceInfo> result;
    for (const DriverEntry& entry : enumerateDrivers()) {
        AudioDeviceInfo info;
        info.id = entry.clsidText;
        info.name = entry.name;
        result.push_back(std::move(info));
    }
    if (!result.empty()) result.front().isDefault = true;
    return result;
}

std::vector<AudioDeviceInfo> AsioDevice::driversWithChannelCounts() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<AudioDeviceInfo> result;
    for (const DriverEntry& entry : enumerateDrivers()) {
        AudioDeviceInfo info;
        info.id = entry.clsidText;
        info.name = entry.name;

        // A driver that is already open elsewhere - including by us - will
        // refuse to initialise. That is not a reason to hide it, so the channel
        // counts simply stay at zero and the name is still offered.
        if (IAsio* driver = createDriver(entry.clsid)) {
            if (driver->init(nullptr) == kTrue) {
                long inputs = 0, outputs = 0;
                if (driver->getChannels(&inputs, &outputs) == kAseOk) {
                    info.inputChannels = static_cast<int>(inputs);
                    info.outputChannels = static_cast<int>(outputs);
                }
            }
            driver->Release();
        }

        result.push_back(std::move(info));
    }

    if (!result.empty()) result.front().isDefault = true;
    return result;
}

bool AsioDevice::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

bool AsioDevice::open(const AudioDeviceSettings& settings, Callback callback) {
    close();

    Impl& impl = *impl_;
    impl.status = AudioDeviceStatus{};

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // -- pick the driver ----------------------------------------------------
    const std::vector<DriverEntry> drivers = enumerateDrivers();
    if (drivers.empty()) {
        impl.status.error = "no ASIO driver is installed";
        return false;
    }

    const DriverEntry* chosen = nullptr;
    for (const DriverEntry& entry : drivers) {
        // Matched on either identifier: settings written before a driver was
        // reinstalled may carry the name rather than the class id.
        if (entry.clsidText == settings.outputDeviceId || entry.name == settings.outputDeviceId) {
            chosen = &entry;
            break;
        }
    }
    if (!chosen) chosen = &drivers.front();

    impl.driver = createDriver(chosen->clsid);
    if (!impl.driver) {
        impl.status.error = chosen->name + ": could not be created (is it 32-bit?)";
        return false;
    }

    // -- initialise ---------------------------------------------------------
    // The window handle is what a driver puts its own dialogs on top of. Null is
    // permitted and is what the specification says to pass when there is not
    // one to hand; every driver in circulation accepts it.
    if (impl.driver->init(nullptr) != kTrue) {
        char message[128] = {};
        impl.driver->getErrorMessage(message);
        impl.status.error = chosen->name + ": " + trimmed(message, sizeof(message));
        if (impl.status.error.size() <= chosen->name.size() + 2)
            impl.status.error = chosen->name + ": the driver refused to initialise";
        close();
        return false;
    }
    impl.driverInitialised = true;

    char nameBuffer[128] = {};
    impl.driver->getDriverName(nameBuffer);
    impl.driverName = trimmed(nameBuffer, sizeof(nameBuffer));
    if (impl.driverName.empty()) impl.driverName = chosen->name;

    // -- channels -----------------------------------------------------------
    long inputs = 0, outputs = 0;
    if (impl.driver->getChannels(&inputs, &outputs) != kAseOk || outputs <= 0) {
        impl.status.error = impl.driverName + ": reported no output channels";
        close();
        return false;
    }

    impl.deviceInputChannels = static_cast<int>(inputs);
    impl.deviceOutputChannels = static_cast<int>(outputs);

    // -- sample rate --------------------------------------------------------
    AsioSampleRate rate = 0.0;
    if (impl.driver->getSampleRate(&rate) != kAseOk || rate < 8000.0 || rate > 768000.0) {
        // Nothing sensible set: ask for the rate the rest of the application
        // defaults to rather than leaving the driver in an undefined state.
        if (impl.driver->setSampleRate(48000.0) == kAseOk) rate = 48000.0;
        else rate = 44100.0;
    }

    // -- buffer size --------------------------------------------------------
    long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    if (impl.driver->getBufferSize(&minimum, &maximum, &preferred, &granularity) != kAseOk
        || preferred <= 0) {
        impl.status.error = impl.driverName + ": would not report a buffer size";
        close();
        return false;
    }

    // The driver's preferred size is the one it is tuned for and the one its own
    // control panel sets. Honour a request only when it is unambiguously legal:
    // a granularity of -1 means powers of two only, and 0 means the preferred
    // size is the only one on offer.
    long requested = static_cast<long>(clampValue(settings.blockSize, 16, 8192));
    long chosenSize = preferred;
    if (requested >= minimum && requested <= maximum) {
        if (granularity == -1) {
            long size = minimum;
            while (size < requested && size < maximum) size *= 2;
            if (size == requested) chosenSize = requested;
        } else if (granularity > 0 && ((requested - minimum) % granularity) == 0) {
            chosenSize = requested;
        }
    }

    // -- channel routing ----------------------------------------------------
    int busChannels = settings.outputChannelCount > 0 ? settings.outputChannelCount
                                                      : std::min(impl.deviceOutputChannels, 2);
    busChannels = clampValue(busChannels, 1, impl.deviceOutputChannels);
    const int offset = clampValue(settings.outputChannelOffset, 0,
                                  std::max(0, impl.deviceOutputChannels - busChannels));

    impl.busChannels = busChannels;
    impl.channelOffset = offset;
    impl.inputChannels = settings.enableInput ? std::min(impl.deviceInputChannels, 32) : 0;

    // -- sample formats -----------------------------------------------------
    // Asked for before the buffers are created, so an unsupported format fails
    // here rather than by writing garbage at the first switch.
    impl.inputTypes.assign(static_cast<std::size_t>(impl.inputChannels), kInt32LSB);
    impl.outputTypes.assign(static_cast<std::size_t>(impl.deviceOutputChannels), kInt32LSB);

    for (int c = 0; c < impl.inputChannels; ++c) {
        AsioChannelInfo info{};
        info.channel = c;
        info.isInput = kTrue;
        if (impl.driver->getChannelInfo(&info) != kAseOk || !isSupportedSampleType(info.type)) {
            impl.status.error = impl.driverName + ": unsupported input sample format ("
                              + sampleTypeName(info.type) + ")";
            close();
            return false;
        }
        impl.inputTypes[static_cast<std::size_t>(c)] = info.type;
    }

    for (int c = 0; c < impl.deviceOutputChannels; ++c) {
        AsioChannelInfo info{};
        info.channel = c;
        info.isInput = kFalse;
        if (impl.driver->getChannelInfo(&info) != kAseOk || !isSupportedSampleType(info.type)) {
            impl.status.error = impl.driverName + ": unsupported output sample format ("
                              + sampleTypeName(info.type) + ")";
            close();
            return false;
        }
        impl.outputTypes[static_cast<std::size_t>(c)] = info.type;
    }

    // -- buffers ------------------------------------------------------------
    // Every output channel is requested, not just the ones in use. A channel
    // with no buffer is one the driver may leave running with stale contents,
    // and silencing it is only possible if we own it.
    impl.bufferInfos.clear();
    impl.bufferInfos.reserve(static_cast<std::size_t>(impl.inputChannels + impl.deviceOutputChannels));

    for (int c = 0; c < impl.inputChannels; ++c)
        impl.bufferInfos.push_back(AsioBufferInfo{ kTrue, c, { nullptr, nullptr } });
    for (int c = 0; c < impl.deviceOutputChannels; ++c)
        impl.bufferInfos.push_back(AsioBufferInfo{ kFalse, c, { nullptr, nullptr } });

    impl.callback = std::move(callback);

    // Published before createBuffers: some drivers call back during it.
    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        g_active = &impl;
    }

    impl.bufferFrames = chosenSize;
    impl.busScratch.assign(static_cast<std::size_t>(chosenSize)
                               * static_cast<std::size_t>(busChannels), 0.0f);
    impl.inputScratch.assign(static_cast<std::size_t>(chosenSize)
                                 * static_cast<std::size_t>(std::max(impl.inputChannels, 1)), 0.0f);

    AsioError created = impl.driver->createBuffers(
        impl.bufferInfos.data(), static_cast<long>(impl.bufferInfos.size()), chosenSize,
        &g_callbacks);

    // A driver within its rights to refuse the requested size still has to be
    // given its preferred one before giving up on it.
    if (created != kAseOk && chosenSize != preferred) {
        chosenSize = preferred;
        impl.bufferFrames = chosenSize;
        impl.busScratch.assign(static_cast<std::size_t>(chosenSize)
                                   * static_cast<std::size_t>(busChannels), 0.0f);
        impl.inputScratch.assign(static_cast<std::size_t>(chosenSize)
                                     * static_cast<std::size_t>(std::max(impl.inputChannels, 1)), 0.0f);
        created = impl.driver->createBuffers(
            impl.bufferInfos.data(), static_cast<long>(impl.bufferInfos.size()), chosenSize,
            &g_callbacks);
    }

    if (created != kAseOk) {
        char message[128] = {};
        impl.driver->getErrorMessage(message);
        impl.status.error = impl.driverName + ": could not create buffers - "
                          + trimmed(message, sizeof(message));
        close();
        return false;
    }
    impl.buffersCreated = true;

    impl.supportsOutputReady = impl.driver->outputReady() == kAseOk;

    // -- latency ------------------------------------------------------------
    long inputLatency = 0, outputLatency = 0;
    impl.driver->getLatencies(&inputLatency, &outputLatency);

    impl.status.running = false;
    impl.status.sampleRate = rate;
    impl.status.blockSize = static_cast<int>(chosenSize);
    impl.status.outputChannels = busChannels;
    impl.status.inputChannels = impl.inputChannels;
    impl.status.deviceOutputChannels = impl.deviceOutputChannels;
    impl.status.outputDeviceName = impl.driverName;
    impl.status.inputDeviceName = impl.inputChannels > 0 ? impl.driverName : std::string();
    impl.status.latencyMilliseconds = rate > 0.0
        ? static_cast<double>(outputLatency) * 1000.0 / rate
        : 0.0;

    // -- go -----------------------------------------------------------------
    if (impl.driver->start() != kAseOk) {
        char message[128] = {};
        impl.driver->getErrorMessage(message);
        impl.status.error = impl.driverName + ": would not start - "
                          + trimmed(message, sizeof(message));
        close();
        return false;
    }

    impl.started = true;
    impl.status.running = true;
    impl.running.store(true, std::memory_order_release);
    return true;
}

void AsioDevice::close() {
    Impl& impl = *impl_;

    impl.running.store(false, std::memory_order_release);

    if (impl.driver) {
        // In order, and all of it before the global is cleared: stop() is what
        // guarantees no callback is in flight, and the callback dereferences the
        // global.
        if (impl.started) impl.driver->stop();
        if (impl.buffersCreated) impl.driver->disposeBuffers();
    }

    {
        std::lock_guard<std::mutex> lock(g_activeMutex);
        if (g_active == &impl) g_active = nullptr;
    }

    if (impl.driver) {
        impl.driver->Release();
        impl.driver = nullptr;
    }

    impl.started = false;
    impl.buffersCreated = false;
    impl.driverInitialised = false;
    impl.supportsOutputReady = false;
    impl.bufferFrames = 0;
    impl.callback = nullptr;
    impl.bufferInfos.clear();
    impl.status.running = false;
    impl.resetRequested.store(false, std::memory_order_release);
}

AudioDeviceStatus AsioDevice::status() const { return impl_->status; }

bool AsioDevice::consumeResetRequest() {
    return impl_->resetRequested.exchange(false, std::memory_order_acq_rel);
}

void AsioDevice::showControlPanel() {
    // Safe while running: the panel is the driver's own window and the driver
    // expects to be asked for it at any point. It raises a reset request if the
    // user changes something that matters.
    if (impl_->driver) impl_->driver->controlPanel();
}

std::string AsioDevice::description() const {
    const AudioDeviceStatus& s = impl_->status;
    if (!s.running) return s.error.empty() ? "ASIO: not running" : "ASIO: " + s.error;

    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "ASIO  %s  %.0f Hz  %d frames  %.1f ms  out %d-%d of %d",
                  s.outputDeviceName.c_str(), s.sampleRate, s.blockSize, s.latencyMilliseconds,
                  impl_->channelOffset + 1, impl_->channelOffset + s.outputChannels,
                  s.deviceOutputChannels);
    return buffer;
}

} // namespace acm::platform
