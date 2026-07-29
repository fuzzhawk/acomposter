// The audio device interface, and the types both backends share.
//
// There are two backends. WASAPI is the one that always works: it is present on
// every Windows 10 machine, it shares the device with whatever else is running,
// and it needs no driver from the manufacturer. ASIO is the one that is worth
// having: it talks to the manufacturer's own driver, which is the only way to
// reach more than the two channels the Windows endpoint mix format exposes, and
// it runs at buffer sizes shared mode will not.
//
// The application does not care which is in use - it opens a device, hands over
// a callback and reads a status - so the difference lives entirely behind this
// interface.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acm::platform {

enum class AudioBackend : int {
    Wasapi = 0,
    Asio = 1,
};

const char* toString(AudioBackend backend) noexcept;
// Parses the string form written into settings.json; unknown values fall back
// to WASAPI, which is the backend that cannot be absent.
AudioBackend audioBackendFromString(const std::string& text) noexcept;

struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool isDefault = false;
    // ASIO drivers report their channel count without being opened, which is
    // worth showing in the picker. Zero means "not known until it is open".
    int outputChannels = 0;
    int inputChannels = 0;
};

struct AudioDeviceSettings {
    AudioBackend backend = AudioBackend::Wasapi;

    // Empty means "whatever Windows considers the default", which is what should
    // happen on a first run. For ASIO it means the first driver on the machine.
    std::string outputDeviceId;
    std::string inputDeviceId;
    bool enableInput = true;
    // Requested buffer size. WASAPI shared mode has its own period and ASIO
    // drivers have their own permitted sizes, so this is a request rather than
    // a guarantee; status().blockSize is what actually happened.
    int blockSize = 256;

    // How many channels the master bus renders, and where they land on the
    // device. Zero means "match the device".
    //
    // Under WASAPI shared mode the endpoint's channel count is fixed, so this is
    // purely a routing choice: it is what lets a 2-channel patch come out of
    // outputs 3/4. Under ASIO the driver exposes every channel it has, so the
    // same two numbers are what open up a 20-output interface.
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
    // What the device itself offers, so the settings UI can bound the choice.
    int deviceOutputChannels = 0;
    std::string outputDeviceName;
    std::string inputDeviceName;
    std::string error;
};

class AudioDevice {
public:
    // Called on the audio thread. `input` is null when there is no capture
    // stream. Both buffers are interleaved: what the backend does to get them
    // into the shape the hardware wants is the backend's business.
    using Callback = std::function<void(const float* input, int inputChannels,
                                        float* output, int outputChannels, int frames)>;

    virtual ~AudioDevice() = default;

    virtual bool open(const AudioDeviceSettings& settings, Callback callback) = 0;
    virtual void close() = 0;

    virtual bool running() const noexcept = 0;
    virtual AudioDeviceStatus status() const = 0;

    // A one-line summary for the status bar.
    virtual std::string description() const = 0;

    // ASIO drivers own their own settings window - buffer size, clock source,
    // channel naming - and there is no way to configure them from outside it.
    // WASAPI has nothing of the sort, hence the default.
    virtual bool hasControlPanel() const noexcept { return false; }
    virtual void showControlPanel() {}

    // True once when the driver has asked to be reset - the user changed the
    // buffer size or the clock source in its control panel, or the hardware was
    // unplugged and put back. The caller reopens the device; doing it inside the
    // driver's own callback would be re-entering it from itself.
    virtual bool consumeResetRequest() { return false; }
};

// -- construction and enumeration -------------------------------------------

std::unique_ptr<AudioDevice> createAudioDevice(AudioBackend backend);

std::vector<AudioDeviceInfo> outputDevices(AudioBackend backend);
std::vector<AudioDeviceInfo> inputDevices(AudioBackend backend);

// False when no ASIO driver is installed, so the settings panel can say that
// rather than offering an empty list.
bool audioBackendAvailable(AudioBackend backend);

} // namespace acm::platform
