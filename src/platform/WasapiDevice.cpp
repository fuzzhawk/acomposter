#include "WasapiDevice.h"

#include "../core/SpscQueue.h"
#include "../core/Types.h"
#include "../core/Utf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

// initguid must come first so the WASAPI CLSIDs and IIDs are defined in this
// translation unit rather than expected from a library that mingw does not ship.
#include <initguid.h>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

namespace acm::platform {
namespace {

template <typename T>
void release(T*& object) {
    if (object) { object->Release(); object = nullptr; }
}

// Ring buffer of interleaved capture frames. Single producer (capture thread),
// single consumer (render thread).
class CaptureRing {
public:
    void prepare(int channels, int capacityFrames) {
        channels_ = std::max(1, channels);
        capacity_ = std::max(1024, capacityFrames);
        buffer_.assign(static_cast<std::size_t>(capacity_) * static_cast<std::size_t>(channels_), 0.0f);
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

    void write(const float* data, int frames) {
        if (frames <= 0 || buffer_.empty()) return;

        std::size_t write = write_.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < channels_; ++c)
                buffer_[write * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(c)] =
                    data[static_cast<std::size_t>(i) * channels_ + c];
            write = (write + 1) % static_cast<std::size_t>(capacity_);
        }
        write_.store(write, std::memory_order_release);
    }

    // Fills `out` with `frames`, padding with silence if the capture stream has
    // not produced enough yet. Under-runs here are normal at start-up and after
    // a glitch; silence is the right answer, not a stall.
    void read(float* out, int frames) {
        if (buffer_.empty()) {
            std::memset(out, 0, sizeof(float) * static_cast<std::size_t>(frames)
                                    * static_cast<std::size_t>(channels_));
            return;
        }

        const std::size_t write = write_.load(std::memory_order_acquire);
        std::size_t read = read_.load(std::memory_order_relaxed);

        const std::size_t available = (write + static_cast<std::size_t>(capacity_) - read)
                                    % static_cast<std::size_t>(capacity_);
        const auto toRead = std::min(static_cast<std::size_t>(frames), available);

        for (std::size_t i = 0; i < toRead; ++i) {
            for (int c = 0; c < channels_; ++c)
                out[i * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(c)] =
                    buffer_[read * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(c)];
            read = (read + 1) % static_cast<std::size_t>(capacity_);
        }
        read_.store(read, std::memory_order_release);

        for (std::size_t i = toRead; i < static_cast<std::size_t>(frames); ++i)
            for (int c = 0; c < channels_; ++c)
                out[i * static_cast<std::size_t>(channels_) + static_cast<std::size_t>(c)] = 0.0f;
    }

    int channels() const noexcept { return channels_; }

private:
    std::vector<float> buffer_;
    int channels_ = 2;
    int capacity_ = 0;
    alignas(64) std::atomic<std::size_t> write_{ 0 };
    alignas(64) std::atomic<std::size_t> read_{ 0 };
};

// The WAVE_FORMAT_EXTENSIBLE sub-format GUIDs, written out rather than pulled
// from ksmedia.h: the symbols live in a static library mingw does not ship, and
// these two values have been fixed since the format was defined.
const GUID kSubFormatPcm =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
const GUID kSubFormatIeeeFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

// What sample format the endpoint actually gave us. Shared mode on Windows 10
// is essentially always 32-bit float, but a driver can still surprise you.
enum class SampleFormat { Float32, Int16, Int24, Int32, Unsupported };

SampleFormat identifyFormat(const WAVEFORMATEX* format) {
    if (!format) return SampleFormat::Unsupported;

    const WAVEFORMATEXTENSIBLE* extensible = nullptr;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22)
        extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);

    const bool isFloat =
        format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
        || (extensible && ::IsEqualGUID(extensible->SubFormat, kSubFormatIeeeFloat));

    if (isFloat) return format->wBitsPerSample == 32 ? SampleFormat::Float32
                                                     : SampleFormat::Unsupported;

    // Anything that is neither float nor explicitly PCM is something we have not
    // been taught to read.
    if (extensible && !::IsEqualGUID(extensible->SubFormat, kSubFormatPcm))
        return SampleFormat::Unsupported;

    switch (format->wBitsPerSample) {
        case 16: return SampleFormat::Int16;
        case 24: return SampleFormat::Int24;
        case 32: return SampleFormat::Int32;
        default: return SampleFormat::Unsupported;
    }
}

void convertToFloat(const std::uint8_t* source, float* destination,
                    int samples, SampleFormat format) {
    switch (format) {
        case SampleFormat::Float32:
            std::memcpy(destination, source, sizeof(float) * static_cast<std::size_t>(samples));
            break;

        case SampleFormat::Int16: {
            const auto* input = reinterpret_cast<const std::int16_t*>(source);
            for (int i = 0; i < samples; ++i) destination[i] = input[i] * (1.0f / 32768.0f);
            break;
        }

        case SampleFormat::Int24:
            for (int i = 0; i < samples; ++i) {
                std::int32_t value = (source[i * 3 + 2] << 16) | (source[i * 3 + 1] << 8) | source[i * 3];
                if (value & 0x800000) value |= ~0xFFFFFF;
                destination[i] = static_cast<float>(value) * (1.0f / 8388608.0f);
            }
            break;

        case SampleFormat::Int32: {
            const auto* input = reinterpret_cast<const std::int32_t*>(source);
            for (int i = 0; i < samples; ++i)
                destination[i] = static_cast<float>(input[i]) * (1.0f / 2147483648.0f);
            break;
        }

        case SampleFormat::Unsupported:
            std::memset(destination, 0, sizeof(float) * static_cast<std::size_t>(samples));
            break;
    }
}

void convertFromFloat(const float* source, std::uint8_t* destination,
                      int samples, SampleFormat format) {
    switch (format) {
        case SampleFormat::Float32:
            std::memcpy(destination, source, sizeof(float) * static_cast<std::size_t>(samples));
            break;

        case SampleFormat::Int16: {
            auto* output = reinterpret_cast<std::int16_t*>(destination);
            for (int i = 0; i < samples; ++i)
                output[i] = static_cast<std::int16_t>(clampValue(source[i], -1.0f, 1.0f) * 32767.0f);
            break;
        }

        case SampleFormat::Int24:
            for (int i = 0; i < samples; ++i) {
                const auto value = static_cast<std::int32_t>(
                    clampValue(source[i], -1.0f, 1.0f) * 8388607.0f);
                destination[i * 3] = static_cast<std::uint8_t>(value & 0xFF);
                destination[i * 3 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
                destination[i * 3 + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
            }
            break;

        case SampleFormat::Int32: {
            auto* output = reinterpret_cast<std::int32_t*>(destination);
            for (int i = 0; i < samples; ++i)
                output[i] = static_cast<std::int32_t>(
                    static_cast<double>(clampValue(source[i], -1.0f, 1.0f)) * 2147483647.0);
            break;
        }

        case SampleFormat::Unsupported:
            std::memset(destination, 0, static_cast<std::size_t>(samples) * 4);
            break;
    }
}

std::string endpointName(IMMDevice* device) {
    IPropertyStore* properties = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return "unknown device";

    PROPVARIANT value;
    ::PropVariantInit(&value);
    std::string name = "unknown device";

    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
        name = wideToUtf8(value.pwszVal);

    ::PropVariantClear(&value);
    properties->Release();
    return name;
}

std::vector<AudioDeviceInfo> enumerate(EDataFlow flow) {
    std::vector<AudioDeviceInfo> devices;

    // The enumeration helpers may be called before the app has initialised COM
    // on this thread, so do it here and undo it on the way out.
    const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ownsCom = SUCCEEDED(comResult);

    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof(IMMDeviceEnumerator),
                                     reinterpret_cast<void**>(&enumerator)))) {
        std::string defaultId;
        IMMDevice* defaultDevice = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(defaultDevice->GetId(&id))) {
                defaultId = wideToUtf8(id);
                ::CoTaskMemFree(id);
            }
            defaultDevice->Release();
        }

        IMMDeviceCollection* collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
            UINT count = 0;
            collection->GetCount(&count);

            for (UINT i = 0; i < count; ++i) {
                IMMDevice* device = nullptr;
                if (FAILED(collection->Item(i, &device))) continue;

                AudioDeviceInfo info;
                LPWSTR id = nullptr;
                if (SUCCEEDED(device->GetId(&id))) {
                    info.id = wideToUtf8(id);
                    ::CoTaskMemFree(id);
                }
                info.name = endpointName(device);
                info.isDefault = !info.id.empty() && info.id == defaultId;
                devices.push_back(std::move(info));

                device->Release();
            }
            collection->Release();
        }
        enumerator->Release();
    }

    if (ownsCom) ::CoUninitialize();
    return devices;
}

} // namespace

// ---------------------------------------------------------------------------

struct WasapiDevice::Impl {
    IMMDeviceEnumerator* enumerator = nullptr;

    IMMDevice* renderDevice = nullptr;
    IAudioClient* renderClient = nullptr;
    IAudioRenderClient* renderService = nullptr;
    WAVEFORMATEX* renderFormat = nullptr;
    HANDLE renderEvent = nullptr;
    SampleFormat renderSampleFormat = SampleFormat::Float32;
    UINT32 renderBufferFrames = 0;

    IMMDevice* captureDevice = nullptr;
    IAudioClient* captureClient = nullptr;
    IAudioCaptureClient* captureService = nullptr;
    WAVEFORMATEX* captureFormat = nullptr;
    HANDLE captureEvent = nullptr;
    SampleFormat captureSampleFormat = SampleFormat::Float32;

    std::thread renderThread;
    std::thread captureThread;
    std::atomic<bool> stopRequested{ false };

    CaptureRing captureRing;
    std::vector<float> captureScratch;
    std::vector<float> renderScratch;   // device layout, handed to WASAPI
    std::vector<float> busScratch;      // what the engine renders into
    std::vector<float> inputScratch;
    int busChannels = 2;
    int channelOffset = 0;

    WasapiDevice::Callback callback;
    AudioDeviceStatus status;

    void renderThreadMain();
    void captureThreadMain();
    void releaseAll();
};

WasapiDevice::WasapiDevice() : impl_(std::make_unique<Impl>()) {}

WasapiDevice::~WasapiDevice() { close(); }

std::vector<AudioDeviceInfo> WasapiDevice::outputDevices() { return enumerate(eRender); }
std::vector<AudioDeviceInfo> WasapiDevice::inputDevices() { return enumerate(eCapture); }

// ---------------------------------------------------------------------------

bool WasapiDevice::open(const AudioDeviceSettings& settings, Callback callback) {
    close();

    impl_->callback = std::move(callback);
    impl_->status = AudioDeviceStatus{};
    impl_->stopRequested.store(false, std::memory_order_relaxed);

    const auto fail = [&](const std::string& message) {
        impl_->status.error = message;
        impl_->releaseAll();
        return false;
    };

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&impl_->enumerator))))
        return fail("could not reach the Windows audio system");

    // -- render ------------------------------------------------------------
    if (settings.outputDeviceId.empty()) {
        if (FAILED(impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &impl_->renderDevice)))
            return fail("no audio output device is available");
    } else {
        if (FAILED(impl_->enumerator->GetDevice(utf8ToWide(settings.outputDeviceId).c_str(),
                                                &impl_->renderDevice)))
            return fail("the selected output device is no longer available");
    }

    impl_->status.outputDeviceName = endpointName(impl_->renderDevice);

    if (FAILED(impl_->renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(&impl_->renderClient))))
        return fail("could not open the output device");

    if (FAILED(impl_->renderClient->GetMixFormat(&impl_->renderFormat)))
        return fail("could not read the output device's format");

    impl_->renderSampleFormat = identifyFormat(impl_->renderFormat);
    if (impl_->renderSampleFormat == SampleFormat::Unsupported)
        return fail("the output device uses a sample format acomposter does not understand");

    // Ask for a buffer at least as long as the requested block, expressed in
    // 100-nanosecond units as WASAPI insists.
    const double requestedSeconds =
        static_cast<double>(clampValue(settings.blockSize, 32, 8192))
        / static_cast<double>(impl_->renderFormat->nSamplesPerSec);
    REFERENCE_TIME requestedDuration = static_cast<REFERENCE_TIME>(requestedSeconds * 1.0e7 * 2.0);

    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    impl_->renderClient->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    requestedDuration = std::max<REFERENCE_TIME>(requestedDuration, defaultPeriod);

    HRESULT result = impl_->renderClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        requestedDuration, 0, impl_->renderFormat, nullptr);

    if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // The documented recovery: ask how big the buffer really needs to be,
        // throw the client away, and start again with that size.
        UINT32 alignedFrames = 0;
        impl_->renderClient->GetBufferSize(&alignedFrames);
        release(impl_->renderClient);

        if (FAILED(impl_->renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                 reinterpret_cast<void**>(&impl_->renderClient))))
            return fail("could not reopen the output device");

        requestedDuration = static_cast<REFERENCE_TIME>(
            1.0e7 * alignedFrames / impl_->renderFormat->nSamplesPerSec + 0.5);
        result = impl_->renderClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            requestedDuration, 0, impl_->renderFormat, nullptr);
    }

    if (FAILED(result))
        return fail("the output device refused the requested buffer size");

    impl_->renderEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->renderEvent || FAILED(impl_->renderClient->SetEventHandle(impl_->renderEvent)))
        return fail("could not set up the output device's callback");

    if (FAILED(impl_->renderClient->GetBufferSize(&impl_->renderBufferFrames)))
        return fail("could not query the output buffer size");

    if (FAILED(impl_->renderClient->GetService(__uuidof(IAudioRenderClient),
                                               reinterpret_cast<void**>(&impl_->renderService))))
        return fail("could not obtain the output render service");

    impl_->status.sampleRate = impl_->renderFormat->nSamplesPerSec;
    impl_->status.deviceOutputChannels = impl_->renderFormat->nChannels;
    impl_->status.blockSize = static_cast<int>(impl_->renderBufferFrames);

    // Clamp the requested routing to what the endpoint actually has, so a patch
    // saved against an 8-output interface still opens on a laptop's stereo jack.
    const int deviceChannels = impl_->renderFormat->nChannels;
    int busChannels = settings.outputChannelCount > 0 ? settings.outputChannelCount : deviceChannels;
    busChannels = clampValue(busChannels, 1, deviceChannels);
    int offset = clampValue(settings.outputChannelOffset, 0, std::max(0, deviceChannels - busChannels));

    impl_->busChannels = busChannels;
    impl_->channelOffset = offset;
    impl_->status.outputChannels = busChannels;

    impl_->renderScratch.assign(static_cast<std::size_t>(impl_->renderBufferFrames)
                                    * static_cast<std::size_t>(deviceChannels), 0.0f);
    impl_->busScratch.assign(static_cast<std::size_t>(impl_->renderBufferFrames)
                                 * static_cast<std::size_t>(busChannels), 0.0f);

    // -- capture (optional) ------------------------------------------------
    if (settings.enableInput) {
        const bool haveDevice =
            settings.inputDeviceId.empty()
                ? SUCCEEDED(impl_->enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                                       &impl_->captureDevice))
                : SUCCEEDED(impl_->enumerator->GetDevice(utf8ToWide(settings.inputDeviceId).c_str(),
                                                         &impl_->captureDevice));

        // No input is a perfectly normal configuration; carry on without it
        // rather than refusing to start.
        if (haveDevice
            && SUCCEEDED(impl_->captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                        reinterpret_cast<void**>(&impl_->captureClient)))
            && SUCCEEDED(impl_->captureClient->GetMixFormat(&impl_->captureFormat))) {

            impl_->captureSampleFormat = identifyFormat(impl_->captureFormat);

            const HRESULT captureResult = impl_->captureClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                    | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                requestedDuration, 0, impl_->captureFormat, nullptr);

            if (SUCCEEDED(captureResult)
                && impl_->captureSampleFormat != SampleFormat::Unsupported) {

                impl_->captureEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (impl_->captureEvent
                    && SUCCEEDED(impl_->captureClient->SetEventHandle(impl_->captureEvent))
                    && SUCCEEDED(impl_->captureClient->GetService(
                           __uuidof(IAudioCaptureClient),
                           reinterpret_cast<void**>(&impl_->captureService)))) {

                    impl_->status.inputChannels = impl_->captureFormat->nChannels;
                    impl_->status.inputDeviceName = endpointName(impl_->captureDevice);

                    // Half a second of slack absorbs the drift between the two
                    // independent device clocks.
                    impl_->captureRing.prepare(impl_->captureFormat->nChannels,
                                               static_cast<int>(impl_->captureFormat->nSamplesPerSec / 2));
                    impl_->captureScratch.assign(
                        static_cast<std::size_t>(impl_->captureFormat->nSamplesPerSec / 10)
                            * impl_->captureFormat->nChannels, 0.0f);
                } else {
                    release(impl_->captureService);
                }
            }
        }

        if (!impl_->captureService) {
            release(impl_->captureClient);
            release(impl_->captureDevice);
            if (impl_->captureFormat) { ::CoTaskMemFree(impl_->captureFormat); impl_->captureFormat = nullptr; }
            if (impl_->captureEvent) { ::CloseHandle(impl_->captureEvent); impl_->captureEvent = nullptr; }
            impl_->status.inputChannels = 0;
        }
    }

    impl_->inputScratch.assign(static_cast<std::size_t>(impl_->renderBufferFrames)
                                   * std::max(1, impl_->status.inputChannels), 0.0f);

    // Round-trip latency is the buffer plus the device's own period; report the
    // buffer, which is the part acomposter controls.
    impl_->status.latencyMilliseconds =
        1000.0 * impl_->renderBufferFrames / impl_->status.sampleRate;
    impl_->status.running = true;

    running_.store(true, std::memory_order_release);

    if (impl_->captureService) {
        impl_->captureClient->Start();
        impl_->captureThread = std::thread(&Impl::captureThreadMain, impl_.get());
    }

    impl_->renderClient->Start();
    impl_->renderThread = std::thread(&Impl::renderThreadMain, impl_.get());

    return true;
}

void WasapiDevice::close() {
    if (!impl_) return;

    impl_->stopRequested.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    // Signalling the events wakes the threads out of their waits immediately
    // rather than after a full buffer period.
    if (impl_->renderEvent) ::SetEvent(impl_->renderEvent);
    if (impl_->captureEvent) ::SetEvent(impl_->captureEvent);

    if (impl_->renderThread.joinable()) impl_->renderThread.join();
    if (impl_->captureThread.joinable()) impl_->captureThread.join();

    if (impl_->renderClient) impl_->renderClient->Stop();
    if (impl_->captureClient) impl_->captureClient->Stop();

    impl_->releaseAll();
    impl_->status.running = false;
}

void WasapiDevice::Impl::releaseAll() {
    release(renderService);
    release(renderClient);
    release(renderDevice);
    release(captureService);
    release(captureClient);
    release(captureDevice);
    release(enumerator);

    if (renderFormat) { ::CoTaskMemFree(renderFormat); renderFormat = nullptr; }
    if (captureFormat) { ::CoTaskMemFree(captureFormat); captureFormat = nullptr; }
    if (renderEvent) { ::CloseHandle(renderEvent); renderEvent = nullptr; }
    if (captureEvent) { ::CloseHandle(captureEvent); captureEvent = nullptr; }
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

void WasapiDevice::Impl::renderThreadMain() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // MMCSS: without it the audio thread competes with everything else on the
    // machine and drops out the moment a browser starts animating something.
    DWORD taskIndex = 0;
    HANDLE task = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const int deviceChannels = renderFormat->nChannels;

    while (!stopRequested.load(std::memory_order_acquire)) {
        // A wait that expires means the device stopped feeding us; there is
        // nothing useful to do but leave.
        if (::WaitForSingleObject(renderEvent, 2000) != WAIT_OBJECT_0) break;
        if (stopRequested.load(std::memory_order_acquire)) break;

        UINT32 padding = 0;
        if (FAILED(renderClient->GetCurrentPadding(&padding))) break;

        const UINT32 available = renderBufferFrames - padding;
        if (available == 0) continue;

        BYTE* deviceBuffer = nullptr;
        if (FAILED(renderService->GetBuffer(available, &deviceBuffer))) break;

        const int frames = static_cast<int>(available);

        if (status.inputChannels > 0)
            captureRing.read(inputScratch.data(), frames);

        if (callback) {
            callback(status.inputChannels > 0 ? inputScratch.data() : nullptr,
                     status.inputChannels,
                     busScratch.data(), busChannels, frames);
        } else {
            std::memset(busScratch.data(), 0,
                        sizeof(float) * static_cast<std::size_t>(frames) * busChannels);
        }

        // Scatter the bus into the device's channels at the chosen offset. Every
        // channel outside the target range is silenced, not left stale.
        std::memset(renderScratch.data(), 0,
                    sizeof(float) * static_cast<std::size_t>(frames) * deviceChannels);
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < busChannels; ++c) {
                renderScratch[static_cast<std::size_t>(i) * deviceChannels + channelOffset + c] =
                    busScratch[static_cast<std::size_t>(i) * busChannels + c];
            }
        }

        convertFromFloat(renderScratch.data(), deviceBuffer, frames * deviceChannels, renderSampleFormat);
        renderService->ReleaseBuffer(available, 0);
    }

    if (task) ::AvRevertMmThreadCharacteristics(task);
    ::CoUninitialize();
}

void WasapiDevice::Impl::captureThreadMain() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE task = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const int channels = captureFormat->nChannels;

    while (!stopRequested.load(std::memory_order_acquire)) {
        if (::WaitForSingleObject(captureEvent, 2000) != WAIT_OBJECT_0) break;
        if (stopRequested.load(std::memory_order_acquire)) break;

        UINT32 packetFrames = 0;
        while (SUCCEEDED(captureService->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            if (FAILED(captureService->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            const std::size_t samples = static_cast<std::size_t>(frames) * channels;
            if (captureScratch.size() < samples) captureScratch.resize(samples);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // The device is telling us it has nothing; writing its buffer
                // would be undefined, so write silence ourselves.
                std::fill_n(captureScratch.begin(), samples, 0.0f);
            } else {
                convertToFloat(data, captureScratch.data(), static_cast<int>(samples),
                               captureSampleFormat);
            }

            captureRing.write(captureScratch.data(), static_cast<int>(frames));
            captureService->ReleaseBuffer(frames);
        }
    }

    if (task) ::AvRevertMmThreadCharacteristics(task);
    ::CoUninitialize();
}

// ---------------------------------------------------------------------------

AudioDeviceStatus WasapiDevice::status() const {
    return impl_ ? impl_->status : AudioDeviceStatus{};
}

std::string WasapiDevice::description() const {
    if (!impl_ || !impl_->status.running)
        return impl_ && !impl_->status.error.empty() ? impl_->status.error : "no audio device";

    const AudioDeviceStatus& s = impl_->status;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s  %.0f Hz  %d frames  %.1f ms  %d in / %d out%s",
                  s.outputDeviceName.c_str(), s.sampleRate, s.blockSize,
                  s.latencyMilliseconds, s.inputChannels, s.outputChannels,
                  s.deviceOutputChannels > s.outputChannels ? "  (routed)" : "");
    return buffer;
}

} // namespace acm::platform
