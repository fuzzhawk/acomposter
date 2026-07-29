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

#include "AudioDevice.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acm::platform {

class WasapiDevice final : public AudioDevice {
public:
    WasapiDevice();
    ~WasapiDevice() override;

    WasapiDevice(const WasapiDevice&) = delete;
    WasapiDevice& operator=(const WasapiDevice&) = delete;

    // Enumerates endpoints. Safe to call before open().
    static std::vector<AudioDeviceInfo> outputDevices();
    static std::vector<AudioDeviceInfo> inputDevices();

    bool open(const AudioDeviceSettings& settings, Callback callback) override;
    void close() override;

    bool running() const noexcept override { return running_.load(std::memory_order_acquire); }
    AudioDeviceStatus status() const override;

    // A one-line summary for the status bar.
    std::string description() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{ false };
};

} // namespace acm::platform
