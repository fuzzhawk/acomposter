// The ASIO binary interface, written from the published specification.
//
// Like Vst2Abi.h this exists so acomposter can host ASIO drivers without
// carrying Steinberg's SDK, which cannot be redistributed. Nothing here is
// copied from it: these are the structure layouts and the vtable order that a
// driver expects, which is what the interface *is*.
//
// Two things about ASIO are unusual enough to be worth stating plainly.
//
// First, a driver is instantiated with CoCreateInstance passing its CLSID as
// *both* the class id and the interface id. There is no registered interface
// for IASIO; the class id is the only identifier there is.
//
// Second, the callbacks are plain function pointers with no user-data argument,
// so a process can host exactly one ASIO driver at a time and the host object
// has to be reachable from a global. That is a property of the interface, not a
// shortcut taken here.
//
// This header is 64-bit only. On x86 the SDK's IASIO methods have no calling
// convention specified, so they come out as __thiscall under MSVC and cannot be
// described portably; on x64 there is only one convention and the question does
// not arise. acomposter.exe is x64, so 64-bit drivers are what it can load.
#pragma once

#include <windows.h>
#include <unknwn.h>

namespace acm::platform::asio {

using AsioBool = long;
using AsioError = long;
using AsioSampleRate = double;

constexpr AsioBool kFalse = 0;
constexpr AsioBool kTrue = 1;

// 64-bit quantities are carried as two 32-bit halves. The specification defines
// them this way on platforms without a native 64-bit type; the size and layout
// are what the driver writes to, so they stay as they are.
struct AsioSamples {
    unsigned long hi;
    unsigned long lo;
};

struct AsioTimeStamp {
    unsigned long hi;
    unsigned long lo;
};

// -- errors -----------------------------------------------------------------

constexpr AsioError kAseOk = 0;
constexpr AsioError kAseSuccess = 0x3f4847a0;
constexpr AsioError kAseNotPresent = -1000;
constexpr AsioError kAseHwMalfunction = -999;
constexpr AsioError kAseInvalidParameter = -998;
constexpr AsioError kAseInvalidMode = -997;
constexpr AsioError kAseSpNotAdvancing = -996;
constexpr AsioError kAseNoClock = -995;
constexpr AsioError kAseNoMemory = -994;

// -- sample types -----------------------------------------------------------

enum AsioSampleType : long {
    kInt16MSB = 0,
    kInt24MSB = 1,
    kInt32MSB = 2,
    kFloat32MSB = 3,
    kFloat64MSB = 4,
    kInt32MSB16 = 8,
    kInt32MSB18 = 9,
    kInt32MSB20 = 10,
    kInt32MSB24 = 11,

    kInt16LSB = 16,
    kInt24LSB = 17,
    kInt32LSB = 18,
    kFloat32LSB = 19,
    kFloat64LSB = 20,
    kInt32LSB16 = 24,
    kInt32LSB18 = 25,
    kInt32LSB20 = 26,
    kInt32LSB24 = 27,

    kDsdInt8LSB1 = 32,
    kDsdInt8MSB1 = 33,
    kDsdInt8NER8 = 40,
};

// -- structures -------------------------------------------------------------

struct AsioClockSource {
    long index;
    long associatedChannel;
    long associatedGroup;
    AsioBool isCurrentSource;
    char name[32];
};

struct AsioChannelInfo {
    long channel;
    AsioBool isInput;
    AsioBool isActive;
    long channelGroup;
    AsioSampleType type;
    char name[32];
};

struct AsioBufferInfo {
    AsioBool isInput;
    long channelNum;
    // Filled in by the driver: the two halves of its double buffer.
    void* buffers[2];
};

struct AsioTimeInfo {
    double speed;
    AsioTimeStamp systemTime;
    AsioSamples samplePosition;
    AsioSampleRate sampleRate;
    unsigned long flags;
    char reserved[12];
};

struct AsioTimeCode {
    double speed;
    AsioSamples timeCodeSamples;
    unsigned long flags;
    char future[64];
};

struct AsioTime {
    long reserved[4];
    AsioTimeInfo timeInfo;
    AsioTimeCode timeCode;
};

struct AsioCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, AsioBool directProcess);
    void (*sampleRateDidChange)(AsioSampleRate sampleRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    AsioTime* (*bufferSwitchTimeInfo)(AsioTime* params, long doubleBufferIndex,
                                      AsioBool directProcess);
};

// -- asioMessage selectors --------------------------------------------------

constexpr long kSelectorSupported = 1;
constexpr long kEngineVersion = 2;
constexpr long kResetRequest = 3;
constexpr long kBufferSizeChange = 4;
constexpr long kResyncRequest = 5;
constexpr long kLatenciesChanged = 6;
constexpr long kSupportsTimeInfo = 7;
constexpr long kSupportsTimeCode = 8;
constexpr long kSupportsInputMonitor = 10;

// -- the interface ----------------------------------------------------------

// The vtable order below is the interface. Every entry must stay exactly where
// it is: a driver calls through slot numbers, not names.
class IAsio : public IUnknown {
public:
    virtual AsioBool init(void* systemHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* text) = 0;
    virtual AsioError start() = 0;
    virtual AsioError stop() = 0;
    virtual AsioError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual AsioError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual AsioError getBufferSize(long* minSize, long* maxSize, long* preferredSize,
                                    long* granularity) = 0;
    virtual AsioError canSampleRate(AsioSampleRate sampleRate) = 0;
    virtual AsioError getSampleRate(AsioSampleRate* sampleRate) = 0;
    virtual AsioError setSampleRate(AsioSampleRate sampleRate) = 0;
    virtual AsioError getClockSources(AsioClockSource* clocks, long* numSources) = 0;
    virtual AsioError setClockSource(long reference) = 0;
    virtual AsioError getSamplePosition(AsioSamples* samplePosition, AsioTimeStamp* timeStamp) = 0;
    virtual AsioError getChannelInfo(AsioChannelInfo* info) = 0;
    virtual AsioError createBuffers(AsioBufferInfo* bufferInfos, long numChannels,
                                    long bufferSize, AsioCallbacks* callbacks) = 0;
    virtual AsioError disposeBuffers() = 0;
    virtual AsioError controlPanel() = 0;
    virtual AsioError future(long selector, void* opt) = 0;
    virtual AsioError outputReady() = 0;
};

// The layouts above are what the driver writes into, so a mistake in one is a
// mistake the compiler cannot otherwise catch.
static_assert(sizeof(AsioSamples) == 8, "ASIO 64-bit quantities are two 32-bit halves");
static_assert(sizeof(AsioTimeStamp) == 8, "ASIO 64-bit quantities are two 32-bit halves");
static_assert(sizeof(AsioClockSource) == 48, "unexpected AsioClockSource layout");
static_assert(sizeof(AsioChannelInfo) == 52, "unexpected AsioChannelInfo layout");
static_assert(sizeof(AsioTimeInfo) == 48, "unexpected AsioTimeInfo layout");
static_assert(sizeof(AsioTimeCode) == 88, "unexpected AsioTimeCode layout");
static_assert(sizeof(AsioTime) == 152, "unexpected AsioTime layout");
static_assert(offsetof(AsioBufferInfo, buffers) == 8, "unexpected AsioBufferInfo layout");

} // namespace acm::platform::asio
