// Saved plugin chains, and the tag defaults built on top of them.
//
// A chain preset is the ordered list of plugins on one stem's rack, each with
// its own state. Saving one and applying it to another stem - or to the same
// stem in a different song - is what turns "the way I treat a bass" from
// something rebuilt each time into something named.
//
// They live in the library folder next to the tags and the entries, as readable
// JSON, so copying a library carries the whole standardisation with it. Plugin
// state is opaque binary and is stored base64; everything else is legible and
// hand-editable, which is the point of keeping it here rather than in a blob.
//
// A plugin is recorded by path *and* by unique id and name. Paths move between
// machines; ids do not. Loading tries the path first and falls back to the id,
// and reports what it could not find rather than silently building a shorter
// chain than the one that was saved.
#pragma once

#include "../core/Json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acm::library {

struct ChainPlugin {
    std::string path;
    std::string name;
    std::int32_t uniqueId = 0;
    bool bridged = false;

    // The plugin's own state chunk. Empty when it had none, in which case the
    // parameter list below is what carries the sound.
    std::vector<std::uint8_t> state;
    std::vector<float> parameters;
};

struct ChainPreset {
    std::string name;
    std::vector<ChainPlugin> plugins;

    bool empty() const noexcept { return plugins.empty(); }
};

class ChainStore {
public:
    // Points at a library root; the chains live in a subdirectory of it.
    void open(const std::string& utf8LibraryRoot);
    bool isOpen() const noexcept { return !directory_.empty(); }

    std::vector<std::string> names() const;
    bool load(const std::string& name, ChainPreset& out) const;
    bool save(const ChainPreset& preset) const;
    bool remove(const std::string& name);

    static JsonValue toJson(const ChainPreset& preset);
    static ChainPreset fromJson(const JsonValue& value);

private:
    std::string pathFor(const std::string& name) const;
    std::string directory_;
};

} // namespace acm::library
