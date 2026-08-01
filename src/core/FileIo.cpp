#include "FileIo.h"

#include "Utf.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <cstdio>
#  include <sys/stat.h>
#  include <dirent.h>
#endif

namespace acm {
namespace {

#ifdef _WIN32
std::string lastErrorText() {
    const DWORD code = ::GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    std::string text = n ? wideToUtf8(std::wstring_view(buffer, n)) : "unknown error";
    if (buffer) ::LocalFree(buffer);

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text + " (" + std::to_string(code) + ")";
}
#endif

void setError(std::string* error, const std::string& text) {
    if (error) *error = text;
}

} // namespace

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool readFileBytes(const std::string& utf8Path, std::vector<std::uint8_t>& out, std::string* error) {
    out.clear();

#ifdef _WIN32
    const std::wstring wide = utf8ToWide(utf8Path);
    HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        setError(error, "could not open '" + utf8Path + "': " + lastErrorText());
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        setError(error, "could not size '" + utf8Path + "': " + lastErrorText());
        ::CloseHandle(file);
        return false;
    }

    // A single allocation this large is almost certainly a mistake (a patch file
    // or a sample), so refuse rather than thrash.
    if (size.QuadPart > (2LL << 30)) {
        setError(error, "'" + utf8Path + "' is larger than 2 GB");
        ::CloseHandle(file);
        return false;
    }

    out.resize(static_cast<std::size_t>(size.QuadPart));

    std::size_t offset = 0;
    while (offset < out.size()) {
        // ReadFile takes a 32-bit count, so large files need several passes.
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, 1u << 24));
        DWORD read = 0;
        if (!::ReadFile(file, out.data() + offset, chunk, &read, nullptr) || read == 0) {
            setError(error, "read failed on '" + utf8Path + "': " + lastErrorText());
            ::CloseHandle(file);
            out.clear();
            return false;
        }
        offset += read;
    }

    ::CloseHandle(file);
    return true;
#else
    std::FILE* file = std::fopen(utf8Path.c_str(), "rb");
    if (!file) {
        setError(error, "could not open '" + utf8Path + "'");
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) { std::fclose(file); return false; }

    out.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);

    if (read != out.size()) {
        setError(error, "short read on '" + utf8Path + "'");
        out.clear();
        return false;
    }
    return true;
#endif
}

bool readFileText(const std::string& utf8Path, std::string& out, std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (!readFileBytes(utf8Path, bytes, error)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

bool writeFileBytes(const std::string& utf8Path, const void* data, std::size_t size, std::string* error) {
    // Write to a sibling temp file and move it into place, so an interrupted
    // save cannot leave a half-written patch where the original used to be.
    const std::string tempPath = utf8Path + ".acptmp";

#ifdef _WIN32
    const std::wstring wideTemp = utf8ToWide(tempPath);
    HANDLE file = ::CreateFileW(wideTemp.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        setError(error, "could not create '" + tempPath + "': " + lastErrorText());
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - offset, 1u << 24));
        DWORD written = 0;
        if (!::WriteFile(file, bytes + offset, chunk, &written, nullptr) || written == 0) {
            setError(error, "write failed on '" + tempPath + "': " + lastErrorText());
            ::CloseHandle(file);
            ::DeleteFileW(wideTemp.c_str());
            return false;
        }
        offset += written;
    }
    ::CloseHandle(file);

    const std::wstring wideTarget = utf8ToWide(utf8Path);
    if (!::MoveFileExW(wideTemp.c_str(), wideTarget.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        setError(error, "could not replace '" + utf8Path + "': " + lastErrorText());
        ::DeleteFileW(wideTemp.c_str());
        return false;
    }
    return true;
#else
    std::FILE* file = std::fopen(tempPath.c_str(), "wb");
    if (!file) {
        setError(error, "could not create '" + tempPath + "'");
        return false;
    }
    const std::size_t written = std::fwrite(data, 1, size, file);
    std::fclose(file);
    if (written != size) {
        std::remove(tempPath.c_str());
        setError(error, "short write on '" + tempPath + "'");
        return false;
    }
    std::remove(utf8Path.c_str());
    if (std::rename(tempPath.c_str(), utf8Path.c_str()) != 0) {
        setError(error, "could not replace '" + utf8Path + "'");
        return false;
    }
    return true;
#endif
}

bool writeFileText(const std::string& utf8Path, std::string_view text, std::string* error) {
    return writeFileBytes(utf8Path, text.data(), text.size(), error);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool fileExists(const std::string& utf8Path) {
#ifdef _WIN32
    const DWORD attributes = ::GetFileAttributesW(utf8ToWide(utf8Path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return ::stat(utf8Path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool directoryExists(const std::string& utf8Path) {
#ifdef _WIN32
    const DWORD attributes = ::GetFileAttributesW(utf8ToWide(utf8Path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return ::stat(utf8Path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::int64_t fileSize(const std::string& utf8Path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(utf8ToWide(utf8Path).c_str(), GetFileExInfoStandard, &data))
        return -1;
    return (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
#else
    struct stat st{};
    if (::stat(utf8Path.c_str(), &st) != 0) return -1;
    return static_cast<std::int64_t>(st.st_size);
#endif
}

std::int64_t fileModifiedTime(const std::string& utf8Path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(utf8ToWide(utf8Path).c_str(), GetFileExInfoStandard, &data))
        return 0;
    return (static_cast<std::int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
         | data.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st{};
    if (::stat(utf8Path.c_str(), &st) != 0) return 0;
    return static_cast<std::int64_t>(st.st_mtime);
#endif
}

bool deleteFile(const std::string& utf8Path) {
    if (utf8Path.empty()) return false;
    if (::DeleteFileW(utf8ToWide(utf8Path).c_str())) return true;
    return ::GetLastError() == ERROR_FILE_NOT_FOUND
        || ::GetLastError() == ERROR_PATH_NOT_FOUND;
}

bool createDirectories(const std::string& utf8Path) {
    if (utf8Path.empty()) return false;
    if (directoryExists(utf8Path)) return true;

    const std::string parent = pathParent(utf8Path);
    if (!parent.empty() && parent != utf8Path && !directoryExists(parent)) {
        if (!createDirectories(parent)) return false;
    }

#ifdef _WIN32
    if (::CreateDirectoryW(utf8ToWide(utf8Path).c_str(), nullptr)) return true;
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return ::mkdir(utf8Path.c_str(), 0755) == 0 || directoryExists(utf8Path);
#endif
}

std::vector<DirectoryEntry> listDirectory(const std::string& utf8Path,
                                          const std::vector<std::string>& extensions) {
    std::vector<DirectoryEntry> entries;

    const auto accepted = [&](const std::string& name, bool isDirectory) {
        if (isDirectory || extensions.empty()) return true;
        const std::string ext = pathExtension(name);
        return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
    };

#ifdef _WIN32
    WIN32_FIND_DATAW findData{};
    const std::wstring pattern = utf8ToWide(pathJoin(utf8Path, "*"));
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &findData);
    if (handle == INVALID_HANDLE_VALUE) return entries;

    do {
        const std::string name = wideToUtf8(findData.cFileName);
        if (name == "." || name == "..") continue;
        if (findData.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;

        DirectoryEntry entry;
        entry.name = name;
        entry.fullPath = pathJoin(utf8Path, name);
        entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.size = (static_cast<std::int64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
        entry.modifiedTime =
            (static_cast<std::int64_t>(findData.ftLastWriteTime.dwHighDateTime) << 32)
            | findData.ftLastWriteTime.dwLowDateTime;

        if (accepted(entry.name, entry.isDirectory)) entries.push_back(std::move(entry));
    } while (::FindNextFileW(handle, &findData));

    ::FindClose(handle);
#else
    DIR* dir = ::opendir(utf8Path.c_str());
    if (!dir) return entries;
    while (dirent* item = ::readdir(dir)) {
        const std::string name = item->d_name;
        if (name == "." || name == ".." || (!name.empty() && name[0] == '.')) continue;

        DirectoryEntry entry;
        entry.name = name;
        entry.fullPath = pathJoin(utf8Path, name);

        struct stat st{};
        if (::stat(entry.fullPath.c_str(), &st) == 0) {
            entry.isDirectory = S_ISDIR(st.st_mode);
            entry.size = static_cast<std::int64_t>(st.st_size);
        }
        if (accepted(entry.name, entry.isDirectory)) entries.push_back(std::move(entry));
    }
    ::closedir(dir);
#endif

    // Directories first, then names, case-insensitively - the order a person
    // expects from a file browser.
    std::sort(entries.begin(), entries.end(), [](const DirectoryEntry& a, const DirectoryEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return std::lexicographical_compare(
            a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
            [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
            });
    });

    return entries;
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

namespace {
bool isSeparator(char c) { return c == '\\' || c == '/'; }
} // namespace

std::string pathJoin(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);

    std::string out(a);
    if (!isSeparator(out.back())) out += '\\';

    std::size_t start = 0;
    while (start < b.size() && isSeparator(b[start])) ++start;
    out.append(b.substr(start));
    return out;
}

std::string pathParent(std::string_view path) {
    std::size_t end = path.size();
    while (end > 0 && isSeparator(path[end - 1])) --end;
    while (end > 0 && !isSeparator(path[end - 1])) --end;
    while (end > 1 && isSeparator(path[end - 1])) --end;
    return std::string(path.substr(0, end));
}

std::string pathLeaf(std::string_view path) {
    std::size_t end = path.size();
    while (end > 0 && isSeparator(path[end - 1])) --end;
    std::size_t start = end;
    while (start > 0 && !isSeparator(path[start - 1])) --start;
    return std::string(path.substr(start, end - start));
}

std::string pathStem(std::string_view path) {
    const std::string leaf = pathLeaf(path);
    const std::size_t dot = leaf.find_last_of('.');
    // A leading dot is part of the name, not an extension separator.
    if (dot == std::string::npos || dot == 0) return leaf;
    return leaf.substr(0, dot);
}

std::string pathExtension(std::string_view path) {
    const std::string leaf = pathLeaf(path);
    const std::size_t dot = leaf.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};

    std::string ext = leaf.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string pathNormalise(std::string_view path) {
    std::string out;
    out.reserve(path.size());

    for (std::size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        if (c == '/') c = '\\';
        // Collapse repeated separators, but keep a leading "\\" so UNC paths
        // survive the round trip.
        if (c == '\\' && !out.empty() && out.back() == '\\' && i != 1) continue;
        out += c;
    }
    return out;
}

} // namespace acm
