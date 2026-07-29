#include "Vst2Plugin.h"

#include "../core/FileIo.h"

namespace acm::vst2 {

const char* toString(Architecture arch) noexcept {
    switch (arch) {
        case Architecture::X86: return "32-bit";
        case Architecture::X64: return "64-bit";
        case Architecture::Unknown: break;
    }
    return "unknown";
}

bool isNativeArchitecture(Architecture arch) noexcept {
    // Decided at compile time by the architecture of whichever binary this is
    // linked into: the host app (x64) or one of the bridge helpers.
    if constexpr (sizeof(void*) == 8)
        return arch == Architecture::X64;
    else
        return arch == Architecture::X86;
}

std::string PluginDescription::identifier() const {
    // The unique id alone is not enough: it collides across the sub-plugins of a
    // shell, and a few badly behaved plugins leave it at zero. Pairing it with
    // the file name keeps patches pointing at the right thing.
    return std::to_string(static_cast<unsigned>(uniqueId)) + ":" + pathLeaf(path);
}

JsonValue toJson(const PluginDescription& description) {
    JsonValue out = JsonValue::object();
    out.set("path", description.path);
    out.set("name", description.name);
    out.set("vendor", description.vendor);
    out.set("product", description.product);
    out.set("category", description.category);
    out.set("uniqueId", static_cast<double>(description.uniqueId));
    out.set("version", static_cast<double>(description.version));
    out.set("vstVersion", static_cast<double>(description.vstVersion));
    out.set("numInputs", description.numInputs);
    out.set("numOutputs", description.numOutputs);
    out.set("numParameters", description.numParameters);
    out.set("numPrograms", description.numPrograms);
    out.set("isSynth", description.isSynth);
    out.set("hasEditor", description.hasEditor);
    out.set("usesChunks", description.usesChunks);
    out.set("architecture", static_cast<int>(description.architecture));
    out.set("fileSize", static_cast<double>(description.fileSize));
    out.set("fileModifiedTime", static_cast<double>(description.fileModifiedTime));
    return out;
}

PluginDescription pluginDescriptionFromJson(const JsonValue& in) {
    PluginDescription description;
    description.path = in.getString("path");
    description.name = in.getString("name");
    description.vendor = in.getString("vendor");
    description.product = in.getString("product");
    description.category = in.getString("category");
    description.uniqueId = static_cast<std::int32_t>(in.getInt64("uniqueId", 0));
    description.version = static_cast<std::int32_t>(in.getInt64("version", 0));
    description.vstVersion = static_cast<std::int32_t>(in.getInt64("vstVersion", 0));
    description.numInputs = in.getInt("numInputs", 0);
    description.numOutputs = in.getInt("numOutputs", 0);
    description.numParameters = in.getInt("numParameters", 0);
    description.numPrograms = in.getInt("numPrograms", 0);
    description.isSynth = in.getBool("isSynth", false);
    description.hasEditor = in.getBool("hasEditor", false);
    description.usesChunks = in.getBool("usesChunks", false);

    const int architecture = in.getInt("architecture", 0);
    description.architecture = (architecture == 1) ? Architecture::X86
                             : (architecture == 2) ? Architecture::X64
                                                   : Architecture::Unknown;

    description.fileSize = in.getInt64("fileSize", 0);
    description.fileModifiedTime = in.getInt64("fileModifiedTime", 0);
    return description;
}

} // namespace acm::vst2
