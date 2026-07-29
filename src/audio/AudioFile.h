// Audio file decoding and encoding, written from scratch.
//
// Supported for reading:
//   WAV  - PCM 8/16/24/32 bit, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE,
//          plus 'smpl' loop points and 'acid' tempo metadata
//   AIFF - PCM 8/16/24/32 bit big-endian, and the AIFC variants 'sowt'
//          (little-endian PCM), 'fl32' and 'fl64'
//
// Supported for writing: WAV, 16/24/32-bit PCM or 32-bit float. That is what the
// looper's "export take" and the master recorder need; nothing else writes audio.
#pragma once

#include "SampleBuffer.h"

#include <memory>
#include <string>
#include <vector>

namespace acm::audiofile {

struct LoadOptions {
    bool buildOverview = true;
    int overviewBuckets = 2048;

    // Scales the file so its loudest peak sits at 0 dBFS. Off by default:
    // relative levels between loaded samples are usually deliberate.
    bool normalise = false;

    // Guard against loading something absurd into RAM by accident.
    double maxDurationSeconds = 60.0 * 30.0;

    // Files are kept at their own rate; the sample player resamples on playback,
    // which is also how it does pitch and speed. Set this to force a conversion
    // at load time instead (0 = keep the file's rate).
    double forceSampleRate = 0.0;
};

// Returns null and fills `error` on failure.
std::shared_ptr<SampleBuffer> load(const std::string& utf8Path,
                                   std::string* error = nullptr,
                                   const LoadOptions& options = {});

// Decodes from memory. `hintName` only feeds the display name and BPM guess.
std::shared_ptr<SampleBuffer> decode(const std::uint8_t* data, std::size_t size,
                                     const std::string& hintName,
                                     std::string* error = nullptr,
                                     const LoadOptions& options = {});

enum class WavFormat { Pcm16, Pcm24, Pcm32, Float32 };

bool writeWav(const std::string& utf8Path, const SampleBuffer& buffer,
              WavFormat format = WavFormat::Pcm24, std::string* error = nullptr);

// Encodes to memory without touching the filesystem.
std::vector<std::uint8_t> encodeWav(const SampleBuffer& buffer, WavFormat format = WavFormat::Pcm24);

// Lowercase, dot-prefixed, for the file browser's filter.
const std::vector<std::string>& supportedExtensions();
bool isSupportedFile(const std::string& utf8Path);

// Pulls a tempo out of a file name such as "amen_174bpm.wav" or "128 - loop.wav".
// Returns 0 when nothing plausible is found.
double guessBpmFromName(const std::string& name);

// Sample-rate conversion used by forceSampleRate and by the looper when a take
// is imported at a different rate.
std::shared_ptr<SampleBuffer> resample(const SampleBuffer& source, double targetRate);

} // namespace acm::audiofile
