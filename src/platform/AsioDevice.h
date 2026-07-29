// ASIO output and input.
//
// This is the backend that reaches past two channels. The Windows endpoint mix
// format is what WASAPI shared mode gives you and on almost every interface it
// is stereo, no matter how many outputs the hardware has; the manufacturer's
// ASIO driver exposes all of them.
//
// One driver at a time, process-wide. ASIO's callbacks carry no user pointer, so
// there is nowhere to put a `this` - the active device has to be reachable from
// a global. Two simultaneous drivers would need two globals and there is no way
// to tell from inside a callback which one is calling, so the interface does not
// support it and neither does this.
#pragma once

#include "AudioDevice.h"

#include <memory>
#include <string>
#include <vector>

namespace acm::platform {

class AsioDevice final : public AudioDevice {
public:
    AsioDevice();
    ~AsioDevice() override;

    AsioDevice(const AsioDevice&) = delete;
    AsioDevice& operator=(const AsioDevice&) = delete;

    // Reads HKLM\SOFTWARE\ASIO. Safe to call before open(), and cheap: it does
    // not load any driver, so a broken one cannot take the enumeration with it.
    static std::vector<AudioDeviceInfo> drivers();
    // Instantiates each driver briefly to ask how many channels it has. Slower
    // and riskier than drivers(), so it is only used when the settings panel is
    // actually open.
    static std::vector<AudioDeviceInfo> driversWithChannelCounts();

    bool open(const AudioDeviceSettings& settings, Callback callback) override;
    void close() override;

    bool running() const noexcept override;
    AudioDeviceStatus status() const override;
    std::string description() const override;

    bool hasControlPanel() const noexcept override { return true; }
    void showControlPanel() override;

    bool consumeResetRequest() override;

    // Public only because the ASIO callbacks are free functions with no user
    // pointer, so they have to be able to name the type they reach through.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace acm::platform
