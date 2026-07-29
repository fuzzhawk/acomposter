#include "AudioFile.h"

#include "../core/FileIo.h"
#include "../dsp/Dsp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace acm::audiofile {
namespace {

// ---------------------------------------------------------------------------
// Byte-level reading
// ---------------------------------------------------------------------------

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool has(std::size_t n) const noexcept { return pos_ + n <= size_; }
    std::size_t position() const noexcept { return pos_; }
    std::size_t remaining() const noexcept { return size_ - pos_; }
    void seek(std::size_t p) noexcept { pos_ = std::min(p, size_); }
    void skip(std::size_t n) noexcept { pos_ = std::min(pos_ + n, size_); }
    const std::uint8_t* at(std::size_t p) const noexcept { return data_ + p; }

    bool matches(const char* fourcc) const noexcept {
        return has(4) && std::memcmp(data_ + pos_, fourcc, 4) == 0;
    }

    void readFourcc(char out[5]) noexcept {
        std::memcpy(out, data_ + pos_, 4);
        out[4] = '\0';
        pos_ += 4;
    }

    std::uint8_t u8() noexcept { return data_[pos_++]; }

    std::uint16_t u16le() noexcept {
        const std::uint16_t v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    std::uint32_t u32le() noexcept {
        const std::uint32_t v = static_cast<std::uint32_t>(data_[pos_])
                              | (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8)
                              | (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16)
                              | (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return v;
    }
    std::uint16_t u16be() noexcept {
        const std::uint16_t v = static_cast<std::uint16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
        pos_ += 2;
        return v;
    }
    std::uint32_t u32be() noexcept {
        const std::uint32_t v = (static_cast<std::uint32_t>(data_[pos_]) << 24)
                              | (static_cast<std::uint32_t>(data_[pos_ + 1]) << 16)
                              | (static_cast<std::uint32_t>(data_[pos_ + 2]) << 8)
                              | static_cast<std::uint32_t>(data_[pos_ + 3]);
        pos_ += 4;
        return v;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

// AIFF stores its sample rate as an 80-bit IEEE 754 extended float, which no
// modern compiler has a type for.
double readExtended80(const std::uint8_t* p) noexcept {
    const int sign = (p[0] & 0x80) ? -1 : 1;
    const int exponent = ((p[0] & 0x7F) << 8) | p[1];

    std::uint64_t mantissa = 0;
    for (int i = 2; i < 10; ++i) mantissa = (mantissa << 8) | p[i];

    if (exponent == 0 && mantissa == 0) return 0.0;
    if (exponent == 0x7FFF) return 0.0; // infinity or NaN: treat as unknown

    return sign * static_cast<double>(mantissa) * std::pow(2.0, exponent - 16383 - 63);
}

// ---------------------------------------------------------------------------
// Sample conversion
// ---------------------------------------------------------------------------

enum class Encoding { PcmU8, PcmS8, PcmS16, PcmS24, PcmS32, Float32, Float64 };

int bytesPerSample(Encoding e) noexcept {
    switch (e) {
        case Encoding::PcmU8:
        case Encoding::PcmS8:    return 1;
        case Encoding::PcmS16:   return 2;
        case Encoding::PcmS24:   return 3;
        case Encoding::PcmS32:
        case Encoding::Float32:  return 4;
        case Encoding::Float64:  return 8;
    }
    return 2;
}

float readSample(const std::uint8_t* p, Encoding encoding, bool bigEndian) noexcept {
    switch (encoding) {
        case Encoding::PcmU8:
            return (static_cast<float>(p[0]) - 128.0f) * (1.0f / 128.0f);

        case Encoding::PcmS8:
            return static_cast<float>(static_cast<std::int8_t>(p[0])) * (1.0f / 128.0f);

        case Encoding::PcmS16: {
            const std::uint16_t raw = bigEndian ? static_cast<std::uint16_t>((p[0] << 8) | p[1])
                                                : static_cast<std::uint16_t>(p[0] | (p[1] << 8));
            return static_cast<float>(static_cast<std::int16_t>(raw)) * (1.0f / 32768.0f);
        }

        case Encoding::PcmS24: {
            std::int32_t raw = bigEndian
                ? ((static_cast<std::int32_t>(p[0]) << 16) | (static_cast<std::int32_t>(p[1]) << 8) | p[2])
                : ((static_cast<std::int32_t>(p[2]) << 16) | (static_cast<std::int32_t>(p[1]) << 8) | p[0]);
            // Sign-extend from 24 to 32 bits.
            if (raw & 0x800000) raw |= ~0xFFFFFF;
            return static_cast<float>(raw) * (1.0f / 8388608.0f);
        }

        case Encoding::PcmS32: {
            const std::uint32_t raw = bigEndian
                ? ((static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16)
                   | (static_cast<std::uint32_t>(p[2]) << 8) | p[3])
                : (static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
                   | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24));
            return static_cast<float>(static_cast<std::int32_t>(raw)) * (1.0f / 2147483648.0f);
        }

        case Encoding::Float32: {
            std::uint8_t bytes[4];
            if (bigEndian) { bytes[0] = p[3]; bytes[1] = p[2]; bytes[2] = p[1]; bytes[3] = p[0]; }
            else           { std::memcpy(bytes, p, 4); }
            float v;
            std::memcpy(&v, bytes, 4);
            return std::isfinite(v) ? v : 0.0f;
        }

        case Encoding::Float64: {
            std::uint8_t bytes[8];
            if (bigEndian) { for (int i = 0; i < 8; ++i) bytes[i] = p[7 - i]; }
            else           { std::memcpy(bytes, p, 8); }
            double v;
            std::memcpy(&v, bytes, 8);
            return std::isfinite(v) ? static_cast<float>(v) : 0.0f;
        }
    }
    return 0.0f;
}

// De-interleaves and converts one contiguous run of frames.
void decodeInto(SampleBuffer& out, const std::uint8_t* src, std::size_t srcBytes,
                int channels, Encoding encoding, bool bigEndian) {
    const int stride = bytesPerSample(encoding);
    const std::int64_t frames = out.frames();

    for (int c = 0; c < channels && c < out.channels(); ++c) {
        float* dst = out.channelForWrite(c);
        for (std::int64_t i = 0; i < frames; ++i) {
            const std::size_t offset =
                (static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                 + static_cast<std::size_t>(c)) * static_cast<std::size_t>(stride);
            dst[i] = (offset + static_cast<std::size_t>(stride) <= srcBytes)
                         ? readSample(src + offset, encoding, bigEndian)
                         : 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

std::shared_ptr<SampleBuffer> decodeWav(Reader& reader, std::size_t size,
                                        const std::string& hintName,
                                        std::string* error, const LoadOptions& options) {
    const auto fail = [error](const char* message) -> std::shared_ptr<SampleBuffer> {
        if (error) *error = message;
        return nullptr;
    };

    reader.seek(12); // past "RIFF" size "WAVE"

    int channels = 0;
    int bits = 0;
    double sampleRate = 0.0;
    std::uint16_t formatTag = 0;
    bool haveFormat = false;

    std::size_t dataOffset = 0;
    std::size_t dataBytes = 0;

    bool hasLoop = false;
    std::int64_t loopStart = 0, loopEnd = 0;
    double acidTempo = 0.0;

    while (reader.has(8)) {
        char id[5];
        reader.readFourcc(id);
        const std::uint32_t chunkSize = reader.u32le();
        const std::size_t chunkStart = reader.position();

        // A truncated final chunk is common in files from hardware recorders;
        // clamp rather than bailing out so the audio still loads.
        const std::size_t available = size - chunkStart;
        const std::size_t usable = std::min<std::size_t>(chunkSize, available);

        if (std::memcmp(id, "fmt ", 4) == 0 && usable >= 16) {
            formatTag = reader.u16le();
            channels = reader.u16le();
            sampleRate = static_cast<double>(reader.u32le());
            reader.skip(4); // average bytes per second
            reader.skip(2); // block align
            bits = reader.u16le();

            if (formatTag == 0xFFFE && usable >= 40) {
                reader.skip(2);  // cbSize
                reader.skip(2);  // valid bits per sample
                reader.skip(4);  // channel mask
                // The first two bytes of the sub-format GUID carry the real tag.
                formatTag = reader.u16le();
            }
            haveFormat = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataOffset = chunkStart;
            dataBytes = usable;
        } else if (std::memcmp(id, "smpl", 4) == 0 && usable >= 36) {
            reader.skip(28); // manufacturer .. SMPTE offset
            const std::uint32_t loopCount = reader.u32le();
            reader.skip(4);  // sampler-specific data size
            if (loopCount > 0 && reader.has(24)) {
                reader.skip(4); // cue point id
                reader.skip(4); // loop type (forward / alternating / backward)
                loopStart = static_cast<std::int64_t>(reader.u32le());
                loopEnd = static_cast<std::int64_t>(reader.u32le());
                hasLoop = loopEnd > loopStart;
            }
        } else if (std::memcmp(id, "acid", 4) == 0 && usable >= 24) {
            // ACIDized loops carry the authored tempo as a float at offset 20.
            reader.skip(20);
            std::uint32_t raw = reader.u32le();
            float tempo;
            std::memcpy(&tempo, &raw, 4);
            if (std::isfinite(tempo) && tempo > 20.0f && tempo < 400.0f)
                acidTempo = static_cast<double>(tempo);
        }

        // Chunks are word-aligned, so an odd size is followed by a pad byte.
        std::size_t next = chunkStart + chunkSize + (chunkSize & 1u);
        if (next <= chunkStart) break; // malformed: zero-size chunk loop
        reader.seek(next);
    }

    if (!haveFormat) return fail("no 'fmt ' chunk: this is not a readable WAV file");
    if (dataBytes == 0) return fail("no audio data in this WAV file");
    if (channels <= 0 || channels > kMaxChannelsPerPort) return fail("unsupported channel count");
    if (sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate) return fail("implausible sample rate");

    Encoding encoding;
    if (formatTag == 3) {
        if (bits == 32) encoding = Encoding::Float32;
        else if (bits == 64) encoding = Encoding::Float64;
        else return fail("unsupported float bit depth");
    } else if (formatTag == 1) {
        switch (bits) {
            case 8:  encoding = Encoding::PcmU8; break;   // WAV 8-bit is unsigned
            case 16: encoding = Encoding::PcmS16; break;
            case 24: encoding = Encoding::PcmS24; break;
            case 32: encoding = Encoding::PcmS32; break;
            default: return fail("unsupported PCM bit depth");
        }
    } else {
        return fail("compressed WAV files are not supported (ADPCM, MP3-in-WAV and similar)");
    }

    const int frameBytes = bytesPerSample(encoding) * channels;
    auto frames = static_cast<std::int64_t>(dataBytes / static_cast<std::size_t>(frameBytes));
    if (frames <= 0) return fail("WAV data chunk contains no complete frames");

    const auto maxFrames = static_cast<std::int64_t>(options.maxDurationSeconds * sampleRate);
    if (maxFrames > 0 && frames > maxFrames) frames = maxFrames;

    auto buffer = std::make_shared<SampleBuffer>(channels, frames, sampleRate);
    decodeInto(*buffer, reader.at(dataOffset), dataBytes, channels, encoding, false);

    if (hasLoop && loopEnd <= frames) {
        buffer->hasEmbeddedLoop = true;
        buffer->loopStart = loopStart;
        buffer->loopEnd = loopEnd;
    }
    buffer->detectedBpm = acidTempo;
    return buffer;
}

// ---------------------------------------------------------------------------
// AIFF / AIFC
// ---------------------------------------------------------------------------

std::shared_ptr<SampleBuffer> decodeAiff(Reader& reader, std::size_t size,
                                         std::string* error, const LoadOptions& options) {
    const auto fail = [error](const char* message) -> std::shared_ptr<SampleBuffer> {
        if (error) *error = message;
        return nullptr;
    };

    reader.seek(12); // past "FORM" size "AIFF"/"AIFC"

    int channels = 0;
    int bits = 0;
    double sampleRate = 0.0;
    std::int64_t frames = 0;
    bool haveCommon = false;
    char compression[5] = "NONE";

    std::size_t dataOffset = 0;
    std::size_t dataBytes = 0;

    while (reader.has(8)) {
        char id[5];
        reader.readFourcc(id);
        const std::uint32_t chunkSize = reader.u32be();
        const std::size_t chunkStart = reader.position();
        const std::size_t usable = std::min<std::size_t>(chunkSize, size - chunkStart);

        if (std::memcmp(id, "COMM", 4) == 0 && usable >= 18) {
            channels = reader.u16be();
            frames = static_cast<std::int64_t>(reader.u32be());
            bits = reader.u16be();
            sampleRate = readExtended80(reader.at(reader.position()));
            reader.skip(10);
            if (usable >= 22) reader.readFourcc(compression);
            haveCommon = true;
        } else if (std::memcmp(id, "SSND", 4) == 0 && usable >= 8) {
            const std::uint32_t offset = reader.u32be();
            reader.skip(4); // block size
            dataOffset = reader.position() + offset;
            dataBytes = usable >= (8 + offset) ? usable - 8 - offset : 0;
        }

        std::size_t next = chunkStart + chunkSize + (chunkSize & 1u);
        if (next <= chunkStart) break;
        reader.seek(next);
    }

    if (!haveCommon) return fail("no 'COMM' chunk: this is not a readable AIFF file");
    if (dataBytes == 0) return fail("no audio data in this AIFF file");
    if (channels <= 0 || channels > kMaxChannelsPerPort) return fail("unsupported channel count");
    if (sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate) return fail("implausible sample rate");

    // 'NONE' and 'twos' are plain big-endian PCM; 'sowt' is the little-endian
    // variant Apple's tools write; the float codes are uncompressed too.
    bool bigEndian = true;
    Encoding encoding = Encoding::PcmS16;
    bool encodingChosen = false;

    if (std::memcmp(compression, "fl32", 4) == 0 || std::memcmp(compression, "FL32", 4) == 0) {
        encoding = Encoding::Float32;
        encodingChosen = true;
    } else if (std::memcmp(compression, "fl64", 4) == 0 || std::memcmp(compression, "FL64", 4) == 0) {
        encoding = Encoding::Float64;
        encodingChosen = true;
    } else if (std::memcmp(compression, "sowt", 4) == 0) {
        bigEndian = false;
    } else if (std::memcmp(compression, "NONE", 4) != 0 && std::memcmp(compression, "twos", 4) != 0) {
        return fail("compressed AIFC files are not supported");
    }

    if (!encodingChosen) {
        switch (bits) {
            case 8:  encoding = Encoding::PcmS8; break;  // AIFF 8-bit is signed
            case 16: encoding = Encoding::PcmS16; break;
            case 24: encoding = Encoding::PcmS24; break;
            case 32: encoding = Encoding::PcmS32; break;
            default: return fail("unsupported AIFF bit depth");
        }
    }

    {
        const int frameBytes = bytesPerSample(encoding) * channels;
        const auto framesInData = static_cast<std::int64_t>(dataBytes / static_cast<std::size_t>(frameBytes));
        if (frames <= 0 || frames > framesInData) frames = framesInData;
        if (frames <= 0) return fail("AIFF sound chunk contains no complete frames");

        const auto maxFrames = static_cast<std::int64_t>(options.maxDurationSeconds * sampleRate);
        if (maxFrames > 0 && frames > maxFrames) frames = maxFrames;

        auto buffer = std::make_shared<SampleBuffer>(channels, frames, sampleRate);
        decodeInto(*buffer, reader.at(dataOffset), dataBytes, channels, encoding, bigEndian);
        return buffer;
    }
}

// ---------------------------------------------------------------------------
// WAV encoding
// ---------------------------------------------------------------------------

void pushU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void pushU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void pushFourcc(std::vector<std::uint8_t>& out, const char* id) {
    out.insert(out.end(), id, id + 4);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const std::vector<std::string>& supportedExtensions() {
    static const std::vector<std::string> extensions = { ".wav", ".wave", ".aif", ".aiff", ".aifc" };
    return extensions;
}

bool isSupportedFile(const std::string& utf8Path) {
    const std::string ext = pathExtension(utf8Path);
    const auto& list = supportedExtensions();
    return std::find(list.begin(), list.end(), ext) != list.end();
}

double guessBpmFromName(const std::string& name) {
    // Look for a run of digits immediately followed by "bpm".
    const std::size_t n = name.size();
    for (std::size_t i = 0; i + 3 <= n; ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != 'b') continue;
        if (std::tolower(static_cast<unsigned char>(name[i + 1])) != 'p') continue;
        if (std::tolower(static_cast<unsigned char>(name[i + 2])) != 'm') continue;

        std::size_t end = i;
        while (end > 0 && (name[end - 1] == ' ' || name[end - 1] == '_' || name[end - 1] == '-')) --end;
        std::size_t start = end;
        while (start > 0 && (std::isdigit(static_cast<unsigned char>(name[start - 1])) || name[start - 1] == '.'))
            --start;

        if (start < end) {
            const double bpm = std::strtod(name.substr(start, end - start).c_str(), nullptr);
            if (bpm >= 20.0 && bpm <= 400.0) return bpm;
        }
    }

    // Fall back to a leading number, the convention used by a lot of loop packs
    // ("128 - deep house groove.wav").
    std::size_t i = 0;
    while (i < n && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i >= 2 && i <= 3) {
        const double bpm = std::strtod(name.substr(0, i).c_str(), nullptr);
        if (bpm >= 60.0 && bpm <= 220.0) return bpm;
    }

    return 0.0;
}

std::shared_ptr<SampleBuffer> decode(const std::uint8_t* data, std::size_t size,
                                     const std::string& hintName,
                                     std::string* error, const LoadOptions& options) {
    if (error) error->clear();

    if (!data || size < 16) {
        if (error) *error = "file is too small to contain audio";
        return nullptr;
    }

    Reader reader(data, size);
    std::shared_ptr<SampleBuffer> buffer;

    if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0) {
        buffer = decodeWav(reader, size, hintName, error, options);
    } else if (std::memcmp(data, "FORM", 4) == 0
               && (std::memcmp(data + 8, "AIFF", 4) == 0 || std::memcmp(data + 8, "AIFC", 4) == 0)) {
        buffer = decodeAiff(reader, size, error, options);
    } else {
        if (error) *error = "unrecognised audio format (acomposter reads WAV and AIFF)";
        return nullptr;
    }

    if (!buffer) return nullptr;

    buffer->displayName = pathStem(hintName);
    if (buffer->detectedBpm <= 0.0)
        buffer->detectedBpm = guessBpmFromName(hintName);

    if (options.forceSampleRate > 0.0
        && std::fabs(options.forceSampleRate - buffer->sampleRate()) > 0.5) {
        auto converted = resample(*buffer, options.forceSampleRate);
        if (converted) {
            converted->sourcePath = buffer->sourcePath;
            converted->displayName = buffer->displayName;
            converted->detectedBpm = buffer->detectedBpm;
            buffer = converted;
        }
    }

    buffer->computePeak();

    if (options.normalise && buffer->peakLevel() > 1.0e-6f) {
        const float scale = 1.0f / buffer->peakLevel();
        for (int c = 0; c < buffer->channels(); ++c) {
            float* d = buffer->channelForWrite(c);
            for (std::int64_t i = 0; i < buffer->frames(); ++i) d[i] *= scale;
        }
        buffer->computePeak();
    }

    if (options.buildOverview)
        buffer->buildOverview(options.overviewBuckets);

    return buffer;
}

std::shared_ptr<SampleBuffer> load(const std::string& utf8Path, std::string* error,
                                   const LoadOptions& options) {
    std::vector<std::uint8_t> bytes;
    if (!readFileBytes(utf8Path, bytes, error)) return nullptr;

    auto buffer = decode(bytes.data(), bytes.size(), pathLeaf(utf8Path), error, options);
    if (buffer) buffer->sourcePath = utf8Path;
    return buffer;
}

std::vector<std::uint8_t> encodeWav(const SampleBuffer& buffer, WavFormat format) {
    const int channels = std::max(1, buffer.channels());
    const std::int64_t frames = std::max<std::int64_t>(0, buffer.frames());

    int bits = 24;
    bool isFloat = false;
    switch (format) {
        case WavFormat::Pcm16:   bits = 16; break;
        case WavFormat::Pcm24:   bits = 24; break;
        case WavFormat::Pcm32:   bits = 32; break;
        case WavFormat::Float32: bits = 32; isFloat = true; break;
    }

    const int sampleBytes = bits / 8;
    const std::uint32_t blockAlign = static_cast<std::uint32_t>(sampleBytes * channels);
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(frames) * blockAlign;

    std::vector<std::uint8_t> out;
    out.reserve(44 + dataBytes);

    pushFourcc(out, "RIFF");
    pushU32(out, 36 + dataBytes);
    pushFourcc(out, "WAVE");

    pushFourcc(out, "fmt ");
    pushU32(out, 16);
    pushU16(out, isFloat ? 3 : 1);
    pushU16(out, static_cast<std::uint16_t>(channels));
    pushU32(out, static_cast<std::uint32_t>(buffer.sampleRate()));
    pushU32(out, static_cast<std::uint32_t>(buffer.sampleRate()) * blockAlign);
    pushU16(out, static_cast<std::uint16_t>(blockAlign));
    pushU16(out, static_cast<std::uint16_t>(bits));

    pushFourcc(out, "data");
    pushU32(out, dataBytes);

    for (std::int64_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            const float v = c < buffer.channels() ? buffer.channel(c)[i] : 0.0f;

            if (isFloat) {
                std::uint32_t raw;
                std::memcpy(&raw, &v, 4);
                pushU32(out, raw);
                continue;
            }

            // Clip before quantising; wrapping a hot sample sounds like a fault.
            const float clipped = clampValue(v, -1.0f, 1.0f);

            switch (bits) {
                case 16: {
                    const auto s = static_cast<std::int16_t>(std::lrint(clipped * 32767.0f));
                    pushU16(out, static_cast<std::uint16_t>(s));
                    break;
                }
                case 24: {
                    const auto s = static_cast<std::int32_t>(std::lrint(clipped * 8388607.0f));
                    out.push_back(static_cast<std::uint8_t>(s & 0xFF));
                    out.push_back(static_cast<std::uint8_t>((s >> 8) & 0xFF));
                    out.push_back(static_cast<std::uint8_t>((s >> 16) & 0xFF));
                    break;
                }
                default: {
                    const auto s = static_cast<std::int32_t>(
                        std::llrint(static_cast<double>(clipped) * 2147483647.0));
                    pushU32(out, static_cast<std::uint32_t>(s));
                    break;
                }
            }
        }
    }

    return out;
}

bool writeWav(const std::string& utf8Path, const SampleBuffer& buffer,
              WavFormat format, std::string* error) {
    const std::vector<std::uint8_t> bytes = encodeWav(buffer, format);
    return writeFileBytes(utf8Path, bytes.data(), bytes.size(), error);
}

std::shared_ptr<SampleBuffer> resample(const SampleBuffer& source, double targetRate) {
    if (source.empty() || targetRate < kMinSampleRate || targetRate > kMaxSampleRate)
        return nullptr;

    const double ratio = source.sampleRate() / targetRate;
    const auto outFrames = static_cast<std::int64_t>(static_cast<double>(source.frames()) / ratio);
    if (outFrames <= 0) return nullptr;

    auto out = std::make_shared<SampleBuffer>(source.channels(), outFrames, targetRate);

    for (int c = 0; c < source.channels(); ++c) {
        const float* src = source.channel(c);
        float* dst = out->channelForWrite(c);
        for (std::int64_t i = 0; i < outFrames; ++i)
            dst[i] = dsp::interpolateHermite(src, source.frames(), static_cast<double>(i) * ratio);
    }

    out->hasEmbeddedLoop = source.hasEmbeddedLoop;
    if (source.hasEmbeddedLoop) {
        out->loopStart = static_cast<std::int64_t>(static_cast<double>(source.loopStart) / ratio);
        out->loopEnd = static_cast<std::int64_t>(static_cast<double>(source.loopEnd) / ratio);
    }
    return out;
}

} // namespace acm::audiofile
