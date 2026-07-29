// WASAPI audio I/O.
//
// Shared mode, event driven. Shared mode rather than exclusive because
// acomposter is meant to sit alongside whatever else is making noise on the
// machine, and because exclusive mode fails outright on a device another
// application already holds - a bad way to start a set.
//
// Capture is optional and runs on its own client and its own event. Its frames
// are handed to the render thread through a lock-free ring, because the two
// streams have independent clocks and will drift; the ring absorbs that.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acm::platform {

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool isDefault = false;
};

struct AudioDeviceSettings {
    // Empty means "whatever Windows considers the default", which is what should
    // happen on a first run.
    std::string outputDeviceId;
    std::string inputDeviceId;
    bool enableInput = true;
    // Requested buffer size. WASAPI shared mode has its own period, so this is
    // the size acomposter processes in, not necessarily the device's.
    int blockSize = 256;

    // How many channels the master bus renders, and where they land on the
    // device. Zero means "match the device". Shared mode fixes the endpoint's
    // own channel count, so this is a routing choice rather than a format one:
    // it is what lets a 2-channel patch come out of outputs 3/4 of an interface.
    int outputChannelCount = 0;
    int outputChannelOffset = 0;
};

struct AudioDeviceStatus {
    bool running = false;
    double sampleRate = 0.0;
    int blockSize = 0;
    int outputChannels = 0;
    int inputChannels = 0;
    double latencyMilliseconds = 0.0;
    // What the endpoint itself offers, so the settings UI can bound the choice.
    int deviceOutputChannels = 0;
    std::string outputDeviceName;
    std::string inputDeviceName;
    std::string error;
};

class WasapiDevice {
public:
    // Called on the audio thread. `input` is null when there is no capture
    // stream. Both buffers are interleaved.
    using Callback = std::function<void(const float* input, int inputChannels,
                                        float* output, int outputChannels, int frames)>;

    WasapiDevice();
    ~WasapiDevice();

    WasapiDevice(const WasapiDevice&) = delete;
    WasapiDevice& operator=(const WasapiDevice&) = delete;

    // Enumerates endpoints. Safe to call before open().
    static std::vector<AudioDeviceInfo> outputDevices();
    static std::vector<AudioDeviceInfo> inputDevices();

    bool open(const AudioDeviceSettings& settings, Callback callback);
    void close();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    AudioDeviceStatus status() const;

    // A one-line summary for the status bar.
    std::string description() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{ false };
};

} // namespace acm::platform
