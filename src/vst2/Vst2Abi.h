// VST 2.x binary interface, declared from the published ABI.
//
// This file is a clean-room description of the calling convention and struct
// layout a VST 2.4 plugin DLL exposes. It contains no Steinberg source: the
// field order, opcode numbers and flag values below are the observable public
// interface every VST2 plugin already implements, restated here so acomposter
// can host them without shipping (or requiring) the Steinberg SDK, which is no
// longer distributed and whose licence forbids redistribution anyway.
//
// See docs/VST2.md for the licensing position in full.
//
// The one rule that matters: the declarations must produce the same memory
// layout the plugin's own compiler produced. That means matching field order and
// letting natural alignment do its work - no packing pragmas, because the
// original has none.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace acm::vst2 {

// The ABI uses a pointer-sized integer for dispatcher values, which is what
// makes 32- and 64-bit plugins binary-incompatible with each other and forces
// the out-of-process bridge.
using VstIntPtr = std::intptr_t;
using VstInt32 = std::int32_t;

struct AEffect;

using AEffectDispatcherProc = VstIntPtr (*)(AEffect* effect, VstInt32 opcode, VstInt32 index,
                                            VstIntPtr value, void* ptr, float opt);
using AEffectProcessProc = void (*)(AEffect* effect, float** inputs, float** outputs, VstInt32 sampleFrames);
using AEffectProcessDoubleProc = void (*)(AEffect* effect, double** inputs, double** outputs, VstInt32 sampleFrames);
using AEffectSetParameterProc = void (*)(AEffect* effect, VstInt32 index, float parameter);
using AEffectGetParameterProc = float (*)(AEffect* effect, VstInt32 index);

using AudioMasterCallback = VstIntPtr (*)(AEffect* effect, VstInt32 opcode, VstInt32 index,
                                          VstIntPtr value, void* ptr, float opt);

// 'VstP' as a big-endian fourcc.
inline constexpr VstInt32 kEffectMagic = 0x56737450;

struct AEffect {
    VstInt32 magic;

    AEffectDispatcherProc dispatcher;
    // The pre-2.4 accumulating process call. Never used by acomposter, but it
    // must stay in the layout because the slot is real.
    AEffectProcessProc processAccumulating;
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;

    VstInt32 numPrograms;
    VstInt32 numParams;
    VstInt32 numInputs;
    VstInt32 numOutputs;
    VstInt32 flags;

    VstIntPtr reservedForHost1;
    VstIntPtr reservedForHost2;

    VstInt32 initialDelay;

    VstInt32 realQualitiesDeprecated;
    VstInt32 offQualitiesDeprecated;
    float ioRatioDeprecated;

    void* object;
    void* user;

    VstInt32 uniqueID;
    VstInt32 version;

    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;

    char future[56];
};

// -- AEffect::flags ---------------------------------------------------------

enum EffectFlags : VstInt32 {
    effFlagsHasEditor          = 1 << 0,
    effFlagsCanReplacing       = 1 << 4,   // implements processReplacing
    effFlagsProgramChunks      = 1 << 5,   // state travels as an opaque chunk
    effFlagsIsSynth            = 1 << 8,
    effFlagsNoSoundInStop      = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12,
};

// -- opcodes sent to the plugin --------------------------------------------

enum EffectOpcode : VstInt32 {
    effOpen = 0,
    effClose,
    effSetProgram,
    effGetProgram,
    effSetProgramName,
    effGetProgramName,
    effGetParamLabel,
    effGetParamDisplay,
    effGetParamName,
    effGetVuDeprecated,
    effSetSampleRate,
    effSetBlockSize,
    effMainsChanged,
    effEditGetRect,
    effEditOpen,
    effEditClose,
    effEditDrawDeprecated,
    effEditMouseDeprecated,
    effEditKeyDeprecated,
    effEditIdle,
    effEditTopDeprecated,
    effEditSleepDeprecated,
    effIdentifyDeprecated,
    effGetChunk,
    effSetChunk,

    effProcessEvents,
    effCanBeAutomated,
    effString2Parameter,
    effGetNumProgramCategoriesDeprecated,
    effGetProgramNameIndexed,
    effCopyProgramDeprecated,
    effConnectInputDeprecated,
    effConnectOutputDeprecated,
    effGetInputProperties,
    effGetOutputProperties,
    effGetPlugCategory,
    effGetCurrentPositionDeprecated,
    effGetDestinationBufferDeprecated,
    effOfflineNotify,
    effOfflinePrepare,
    effOfflineRun,
    effProcessVarIo,
    effSetSpeakerArrangement,
    effSetBlockSizeAndSampleRateDeprecated,
    effSetBypass,
    effGetEffectName,
    effGetErrorTextDeprecated,
    effGetVendorString,
    effGetProductString,
    effGetVendorVersion,
    effVendorSpecific,
    effCanDo,
    effGetTailSize,
    effIdleDeprecated,
    effGetIconDeprecated,
    effSetViewPositionDeprecated,
    effGetParameterProperties,
    effKeysRequiredDeprecated,
    effGetVstVersion,
    effEditKeyDown,
    effEditKeyUp,
    effSetEditKnobMode,
    effGetMidiProgramName,
    effHasMidiProgramsChanged,
    effGetMidiProgramCategory,
    effGetMidiKeyName,
    effBeginSetProgram,
    effEndSetProgram,
    effGetSpeakerArrangement,
    effShellGetNextPlugin,
    effStartProcess,
    effStopProcess,
    effSetTotalSampleToProcess,
    effSetPanLaw,
    effBeginLoadBank,
    effBeginLoadProgram,
    effSetProcessPrecision,
    effGetNumMidiInputChannels,
    effGetNumMidiOutputChannels,
};

// -- opcodes the plugin sends back to us -----------------------------------

enum HostOpcode : VstInt32 {
    audioMasterAutomate = 0,
    audioMasterVersion,
    audioMasterCurrentId,
    audioMasterIdle,
    audioMasterPinConnectedDeprecated,
    audioMasterUnused5,
    audioMasterWantMidiDeprecated,
    audioMasterGetTime,
    audioMasterProcessEvents,
    audioMasterSetTimeDeprecated,
    audioMasterTempoAtDeprecated,
    audioMasterGetNumAutomatableParametersDeprecated,
    audioMasterGetParameterQuantizationDeprecated,
    audioMasterIOChanged,
    audioMasterNeedIdleDeprecated,
    audioMasterSizeWindow,
    audioMasterGetSampleRate,
    audioMasterGetBlockSize,
    audioMasterGetInputLatency,
    audioMasterGetOutputLatency,
    audioMasterGetPreviousPlugDeprecated,
    audioMasterGetNextPlugDeprecated,
    audioMasterWillReplaceOrAccumulateDeprecated,
    audioMasterGetCurrentProcessLevel,
    audioMasterGetAutomationState,
    audioMasterOfflineStart,
    audioMasterOfflineRead,
    audioMasterOfflineWrite,
    audioMasterOfflineGetCurrentPass,
    audioMasterOfflineGetCurrentMetaPass,
    audioMasterSetOutputSampleRateDeprecated,
    audioMasterGetOutputSpeakerArrangementDeprecated,
    audioMasterGetVendorString,
    audioMasterGetProductString,
    audioMasterGetVendorVersion,
    audioMasterVendorSpecific,
    audioMasterSetIconDeprecated,
    audioMasterCanDo,
    audioMasterGetLanguage,
    audioMasterOpenWindowDeprecated,
    audioMasterCloseWindowDeprecated,
    audioMasterGetDirectory,
    audioMasterUpdateDisplay,
    audioMasterBeginEdit,
    audioMasterEndEdit,
    audioMasterOpenFileSelector,
    audioMasterCloseFileSelector,
};

// The VST version we advertise: 2.4.
inline constexpr VstInt32 kVstVersion = 2400;

// -- timing ----------------------------------------------------------------

struct VstTimeInfo {
    double samplePos = 0.0;
    double sampleRate = 0.0;
    double nanoSeconds = 0.0;
    double ppqPos = 0.0;
    double tempo = 0.0;
    double barStartPos = 0.0;
    double cycleStartPos = 0.0;
    double cycleEndPos = 0.0;
    VstInt32 timeSigNumerator = 4;
    VstInt32 timeSigDenominator = 4;
    VstInt32 smpteOffset = 0;
    VstInt32 smpteFrameRate = 0;
    VstInt32 samplesToNextClock = 0;
    VstInt32 flags = 0;
};

enum VstTimeInfoFlags : VstInt32 {
    kVstTransportChanged     = 1 << 0,
    kVstTransportPlaying     = 1 << 1,
    kVstTransportCycleActive = 1 << 2,
    kVstTransportRecording   = 1 << 3,
    kVstAutomationWriting    = 1 << 6,
    kVstAutomationReading    = 1 << 7,
    kVstNanosValid           = 1 << 8,
    kVstPpqPosValid          = 1 << 9,
    kVstTempoValid           = 1 << 10,
    kVstBarsValid            = 1 << 11,
    kVstCyclePosValid        = 1 << 12,
    kVstTimeSigValid         = 1 << 13,
    kVstSmpteValid           = 1 << 14,
    kVstClockValid           = 1 << 15,
};

// -- events ----------------------------------------------------------------

enum VstEventType : VstInt32 {
    kVstMidiType = 1,
    kVstAudioTypeDeprecated,
    kVstVideoTypeDeprecated,
    kVstParameterTypeDeprecated,
    kVstTriggerTypeDeprecated,
    kVstSysExType,
};

struct VstEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    char data[16];
};

struct VstMidiEvent {
    VstInt32 type;          // kVstMidiType
    VstInt32 byteSize;      // sizeof(VstMidiEvent)
    VstInt32 deltaFrames;   // offset into the current block
    VstInt32 flags;
    VstInt32 noteLength;
    VstInt32 noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

// A variable-length array of event pointers. The declared size of one is a
// deliberate under-count in the original ABI; hosts allocate a larger block and
// cast, which is what VstEventBlock below does.
struct VstEvents {
    VstInt32 numEvents;
    VstIntPtr reserved;
    VstEvent* events[2];
};

// -- pin and parameter description -----------------------------------------

struct VstPinProperties {
    char label[64];
    VstInt32 flags;
    VstInt32 arrangementType;
    char shortLabel[8];
    char future[48];
};

enum VstPinPropertiesFlags : VstInt32 {
    kVstPinIsActive = 1 << 0,
    kVstPinIsStereo = 1 << 1,
    kVstPinUseSpeaker = 1 << 2,
};

struct VstParameterProperties {
    float stepFloat;
    float smallStepFloat;
    float largeStepFloat;
    char label[64];
    VstInt32 flags;
    VstInt32 minInteger;
    VstInt32 maxInteger;
    VstInt32 stepInteger;
    VstInt32 largeStepInteger;
    char shortLabel[8];

    // The original carries four int16 fields here (display index, category,
    // number in category, reserved). acomposter never reads them; they are
    // declared so the struct is the right size when a plugin fills it in.
    std::int16_t displayIndex;
    std::int16_t category;
    std::int16_t numParametersInCategory;
    std::int16_t reserved;

    char categoryLabel[24];
    char future[16];
};

// -- helpers ---------------------------------------------------------------

// The dispatcher writes C strings into caller-provided buffers of, by
// convention, at most 64 bytes, and is not required to null-terminate.
inline std::string fromVstString(const char* buffer, std::size_t capacity) {
    std::size_t length = 0;
    while (length < capacity && buffer[length] != '\0') ++length;
    return std::string(buffer, length);
}

// A VstEvents block sized for a real number of events.
template <int Capacity>
struct VstEventBlock {
    VstInt32 numEvents = 0;
    VstIntPtr reserved = 0;
    VstEvent* eventPointers[Capacity] = {};
    VstMidiEvent storage[Capacity] = {};

    void clear() noexcept { numEvents = 0; }

    bool add(unsigned char status, unsigned char data1, unsigned char data2, int deltaFrames) noexcept {
        if (numEvents >= Capacity) return false;

        VstMidiEvent& event = storage[numEvents];
        std::memset(&event, 0, sizeof(event));
        event.type = kVstMidiType;
        event.byteSize = static_cast<VstInt32>(sizeof(VstMidiEvent));
        event.deltaFrames = deltaFrames;
        event.midiData[0] = static_cast<char>(status);
        event.midiData[1] = static_cast<char>(data1);
        event.midiData[2] = static_cast<char>(data2);

        eventPointers[numEvents] = reinterpret_cast<VstEvent*>(&event);
        ++numEvents;
        return true;
    }

    // Reinterpreted as the ABI's VstEvents when handed to effProcessEvents; the
    // leading fields line up exactly.
    VstEvents* asVstEvents() noexcept { return reinterpret_cast<VstEvents*>(this); }
};

} // namespace acm::vst2
