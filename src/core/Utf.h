// UTF-8 <-> UTF-16 conversion.
//
// Hand-rolled rather than routed through MultiByteToWideChar so the conversion
// is identical everywhere and can be exercised off-Windows. Everything above the
// platform layer stores text as UTF-8; only Win32 calls see UTF-16.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace acm {

inline std::wstring utf8ToWide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());

    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        int extra = 0;

        if (c < 0x80)              { cp = c;          extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
        else { ++i; continue; } // stray continuation byte

        if (i + static_cast<std::size_t>(extra) >= s.size()) break;

        bool valid = true;
        for (int k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        i += static_cast<std::size_t>(extra) + 1;
        if (!valid) continue;

        if (cp >= 0x10000) {
            // wchar_t is 16 bits on Windows, so non-BMP needs a surrogate pair.
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

inline std::string wideToUtf8(std::wstring_view s) {
    std::string out;
    out.reserve(s.size() * 2);

    for (std::size_t i = 0; i < s.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint32_t>(static_cast<std::uint16_t>(s[i]));

        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            const auto low = static_cast<std::uint32_t>(static_cast<std::uint16_t>(s[i + 1]));
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }

        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Number of code points, for cursor movement in text fields.
inline std::size_t utf8Length(std::string_view s) {
    std::size_t n = 0;
    for (char ch : s)
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;
    return n;
}

// Byte offset of the start of the code point before `offset`, for backspace.
inline std::size_t utf8PrevOffset(std::string_view s, std::size_t offset) {
    if (offset == 0) return 0;
    --offset;
    while (offset > 0 && (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) --offset;
    return offset;
}

// Byte offset of the start of the code point after `offset`, for delete.
inline std::size_t utf8NextOffset(std::string_view s, std::size_t offset) {
    if (offset >= s.size()) return s.size();
    ++offset;
    while (offset < s.size() && (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) ++offset;
    return offset;
}

} // namespace acm
