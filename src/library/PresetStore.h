// A folder of named JSON documents.
//
// Whatever the caller puts in comes back out; this knows nothing about what a
// preset contains. That is the point - the colour node already knows how to
// write and re-bind its own presets, and wrapping that in a store that also
// understood colour targets would be the same knowledge in two places.
//
// The display name lives inside the document rather than in the file name,
// because a file name has been through sanitising and a preset called
// "drums: bus / glue" has to come back with its punctuation.
#pragma once

#include "../core/Json.h"

#include <string>
#include <vector>

namespace acm::library {

class PresetStore {
public:
    // `subdirectory` is created under the library root if it is missing.
    void open(const std::string& utf8LibraryRoot, std::string subdirectory);
    bool isOpen() const noexcept { return !directory_.empty(); }
    const std::string& directory() const noexcept { return directory_; }

    std::vector<std::string> names() const;
    bool load(const std::string& name, JsonValue& out) const;
    // The document is written with its name inside it, so `names()` can report
    // it back exactly as it was given.
    bool save(const std::string& name, JsonValue document) const;
    bool remove(const std::string& name);
    bool exists(const std::string& name) const;

    // Writes a preset only when nothing of that name is there. Used to seed the
    // shipped defaults on first run without ever overwriting an edited one.
    bool saveIfAbsent(const std::string& name, JsonValue document) const;

private:
    std::string pathFor(const std::string& name) const;
    std::string directory_;
};

} // namespace acm::library
