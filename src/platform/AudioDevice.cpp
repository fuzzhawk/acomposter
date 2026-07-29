#include "AudioDevice.h"

#include "AsioDevice.h"
#include "WasapiDevice.h"

namespace acm::platform {

const char* toString(AudioBackend backend) noexcept {
    switch (backend) {
        case AudioBackend::Asio: return "asio";
        case AudioBackend::Wasapi:
        default: return "wasapi";
    }
}

AudioBackend audioBackendFromString(const std::string& text) noexcept {
    if (text == "asio") return AudioBackend::Asio;
    return AudioBackend::Wasapi;
}

std::unique_ptr<AudioDevice> createAudioDevice(AudioBackend backend) {
    switch (backend) {
        case AudioBackend::Asio: return std::make_unique<AsioDevice>();
        case AudioBackend::Wasapi:
        default: return std::make_unique<WasapiDevice>();
    }
}

std::vector<AudioDeviceInfo> outputDevices(AudioBackend backend) {
    switch (backend) {
        // Channel counts are worth the cost here: choosing an interface by name
        // alone tells you nothing about whether it is the one with 20 outputs.
        case AudioBackend::Asio: return AsioDevice::driversWithChannelCounts();
        case AudioBackend::Wasapi:
        default: return WasapiDevice::outputDevices();
    }
}

std::vector<AudioDeviceInfo> inputDevices(AudioBackend backend) {
    switch (backend) {
        // An ASIO driver is one device with both directions; there is no
        // separate input to pick, so the output list is the whole answer.
        case AudioBackend::Asio: return AsioDevice::drivers();
        case AudioBackend::Wasapi:
        default: return WasapiDevice::inputDevices();
    }
}

bool audioBackendAvailable(AudioBackend backend) {
    switch (backend) {
        case AudioBackend::Asio: return !AsioDevice::drivers().empty();
        case AudioBackend::Wasapi:
        default: return true;
    }
}

} // namespace acm::platform
