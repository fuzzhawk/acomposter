#include "PeArchitecture.h"

#include "../core/FileIo.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace acm::vst2 {
namespace {

// PE layout, only as much as we need:
//
//   0x00  "MZ"                       DOS header
//   0x3C  uint32 e_lfanew            file offset of the PE header
//   +0    "PE\0\0"                   signature
//   +4    uint16 Machine             the field we are here for
//   +18   uint16 Characteristics     bit 0x2000 marks a DLL
constexpr std::uint16_t kMachineI386 = 0x014C;
constexpr std::uint16_t kMachineAmd64 = 0x8664;
constexpr std::uint16_t kCharacteristicsDll = 0x2000;

std::uint16_t readU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

std::uint32_t readU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset])
         | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
         | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
         | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

// Reads only the leading bytes, so scanning a folder of 60 MB plugins does not
// pull hundreds of megabytes through the page cache.
bool readHeaderBytes(const std::string& utf8Path, std::vector<std::uint8_t>& out, std::string* error) {
    if (!readFileBytes(utf8Path, out, error)) return false;
    if (out.size() < 0x40) {
        if (error) *error = "file is too small to be a PE image";
        return false;
    }
    return true;
}

struct PeHeaderFields {
    std::uint16_t machine = 0;
    std::uint16_t characteristics = 0;
    bool valid = false;
};

PeHeaderFields readPeHeader(const std::string& utf8Path, std::string* error) {
    PeHeaderFields fields;

    std::vector<std::uint8_t> data;
    if (!readHeaderBytes(utf8Path, data, error)) return fields;

    if (data[0] != 'M' || data[1] != 'Z') {
        if (error) *error = "not a Windows binary (no MZ signature)";
        return fields;
    }

    const std::uint32_t peOffset = readU32(data, 0x3C);
    if (peOffset + 24 > data.size()) {
        if (error) *error = "PE header offset points past the end of the file";
        return fields;
    }

    if (std::memcmp(data.data() + peOffset, "PE\0\0", 4) != 0) {
        if (error) *error = "not a PE image";
        return fields;
    }

    fields.machine = readU16(data, peOffset + 4);
    fields.characteristics = readU16(data, peOffset + 22);
    fields.valid = true;
    return fields;
}

} // namespace

Architecture readPeArchitecture(const std::string& utf8Path, std::string* error) {
    const PeHeaderFields fields = readPeHeader(utf8Path, error);
    if (!fields.valid) return Architecture::Unknown;

    switch (fields.machine) {
        case kMachineI386:  return Architecture::X86;
        case kMachineAmd64: return Architecture::X64;
        default:
            if (error) *error = "unsupported target architecture in PE header";
            return Architecture::Unknown;
    }
}

bool isDynamicLibrary(const std::string& utf8Path) {
    const PeHeaderFields fields = readPeHeader(utf8Path, nullptr);
    return fields.valid && (fields.characteristics & kCharacteristicsDll) != 0;
}

} // namespace acm::vst2
