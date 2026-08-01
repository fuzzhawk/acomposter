// Whole-file read and write with UTF-8 paths.
//
// The rest of the codebase never opens a file itself. Routing everything through
// here means unicode paths work on Windows (CreateFileW, not the ANSI code
// page) while the same code still builds and runs on a host toolchain for tests.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acm {

// Fills `out` with the whole file. Returns false and sets `error` on failure.
bool readFileBytes(const std::string& utf8Path, std::vector<std::uint8_t>& out, std::string* error = nullptr);
bool readFileText(const std::string& utf8Path, std::string& out, std::string* error = nullptr);

bool writeFileBytes(const std::string& utf8Path, const void* data, std::size_t size, std::string* error = nullptr);
bool writeFileText(const std::string& utf8Path, std::string_view text, std::string* error = nullptr);

bool fileExists(const std::string& utf8Path);
bool directoryExists(const std::string& utf8Path);
bool createDirectories(const std::string& utf8Path);
// Removes a file. True when it is gone afterwards, including when it was never
// there - the caller almost always wants "make sure this does not exist".
bool deleteFile(const std::string& utf8Path);
std::int64_t fileSize(const std::string& utf8Path);
// Last-write time as an opaque monotonic stamp; only used to compare against a
// previously recorded value (the plugin scan cache).
std::int64_t fileModifiedTime(const std::string& utf8Path);

// Non-recursive listing. `extensions` filters by lowercase suffix including the
// dot (".wav"); an empty list matches everything.
struct DirectoryEntry {
    std::string name;      // leaf name
    std::string fullPath;
    bool isDirectory = false;
    std::int64_t size = 0;
    // Last write time, in whatever units the platform counts in. Only ever
    // compared against a previously recorded value for the same file, so the
    // epoch does not matter - only that it changes when the file does.
    std::int64_t modifiedTime = 0;
};

std::vector<DirectoryEntry> listDirectory(const std::string& utf8Path,
                                          const std::vector<std::string>& extensions = {});

// -- path helpers (pure string manipulation, no filesystem access) ----------

std::string pathJoin(std::string_view a, std::string_view b);
std::string pathParent(std::string_view path);
std::string pathLeaf(std::string_view path);
std::string pathStem(std::string_view path);            // leaf without extension
std::string pathExtension(std::string_view path);       // lowercase, includes the dot
std::string pathNormalise(std::string_view path);       // forward slashes to backslashes, collapse dupes

} // namespace acm
