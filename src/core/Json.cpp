#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace acm {
namespace {

const JsonValue kNullValue{};

void appendEscaped(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // UTF-8 bytes pass through untouched.
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void appendNumber(std::string& out, double v) {
    if (!std::isfinite(v)) { out += "0"; return; }

    // Integers print without a decimal point so ids and counts stay readable.
    if (v == std::floor(v) && std::fabs(v) < 1.0e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out += buf;
        return;
    }

    // %.17g round-trips exactly but is ugly; try progressively shorter forms and
    // keep the first that reads back identically.
    char buf[40];
    for (int precision = 6; precision <= 17; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    out += buf;
}

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ---------------------------------------------------------------------------
// Recursive-descent parser
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(std::string_view text) : s_(text) {}

    bool parseValue(JsonValue& out) {
        skipWhitespace();
        if (pos_ >= s_.size()) return fail("unexpected end of input");

        switch (s_[pos_]) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = JsonValue(std::move(str));
                return true;
            }
            case 't':
                if (!literal("true")) return false;
                out = JsonValue(true);
                return true;
            case 'f':
                if (!literal("false")) return false;
                out = JsonValue(false);
                return true;
            case 'n':
                if (!literal("null")) return false;
                out = JsonValue();
                return true;
            default: return parseNumber(out);
        }
    }

    bool atEndAfterWhitespace() {
        skipWhitespace();
        return pos_ >= s_.size();
    }

    const std::string& error() const { return error_; }

private:
    bool fail(const char* msg) {
        if (error_.empty()) {
            // Report a line/column rather than a byte offset - patches get
            // hand-edited and an offset into a 200 KB file helps nobody.
            std::size_t line = 1, col = 1;
            for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
                if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "line %zu, column %zu: %s", line, col, msg);
            error_ = buf;
        }
        return false;
    }

    void skipWhitespace() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
            // Line comments are not standard JSON, but they are invaluable in a
            // config format people are expected to read and annotate.
            if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    bool literal(const char* lit) {
        const std::size_t n = std::string_view(lit).size();
        if (s_.substr(pos_, n) != std::string_view(lit)) return fail("bad literal");
        pos_ += n;
        return true;
    }

    bool parseNumber(JsonValue& out) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') ++pos_;
            else break;
        }
        if (pos_ == start) return fail("expected a value");

        const std::string token(s_.substr(start, pos_ - start));
        char* end = nullptr;
        const double v = std::strtod(token.c_str(), &end);
        if (end == token.c_str()) return fail("malformed number");
        out = JsonValue(v);
        return true;
    }

    bool parseString(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected a string");
        ++pos_;
        out.clear();
        while (true) {
            if (pos_ >= s_.size()) return fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }

            if (pos_ >= s_.size()) return fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    // Recombine surrogate pairs so non-BMP characters survive.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size()
                        && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        const std::size_t save = pos_;
                        pos_ += 2;
                        unsigned low = 0;
                        if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        else
                            pos_ = save;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("unknown escape sequence");
            }
        }
    }

    bool hex4(unsigned& out) {
        if (pos_ + 4 > s_.size()) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
        }
        return true;
    }

    bool parseArray(JsonValue& out) {
        if (++depth_ > kMaxDepth) return fail("nesting too deep");
        ++pos_; // '['
        out = JsonValue::array();
        skipWhitespace();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; --depth_; return true; }

        while (true) {
            JsonValue element;
            if (!parseValue(element)) return false;
            out.push(std::move(element));
            skipWhitespace();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; --depth_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(JsonValue& out) {
        if (++depth_ > kMaxDepth) return fail("nesting too deep");
        ++pos_; // '{'
        out = JsonValue::object();
        skipWhitespace();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; --depth_; return true; }

        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
            ++pos_;

            JsonValue value;
            if (!parseValue(value)) return false;
            out.set(key, std::move(value));

            skipWhitespace();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; --depth_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    static constexpr int kMaxDepth = 200;

    std::string_view s_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    std::string error_;
};

} // namespace

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool JsonValue::asBool(bool def) const noexcept {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return num_ != 0.0;
    return def;
}

double JsonValue::asDouble(double def) const noexcept {
    if (type_ == Type::Number) return num_;
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    return def;
}

int JsonValue::asInt(int def) const noexcept {
    if (type_ == Type::Number) return static_cast<int>(std::llround(num_));
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    return def;
}

std::int64_t JsonValue::asInt64(std::int64_t def) const noexcept {
    if (type_ == Type::Number) return static_cast<std::int64_t>(std::llround(num_));
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    return def;
}

std::string JsonValue::asString(std::string_view def) const {
    if (type_ == Type::String) return str_;
    return std::string(def);
}

std::size_t JsonValue::size() const noexcept {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const JsonValue& JsonValue::at(std::size_t i) const {
    if (type_ == Type::Array && i < arr_.size()) return arr_[i];
    return kNullValue;
}

JsonValue& JsonValue::at(std::size_t i) {
    if (type_ != Type::Array) { type_ = Type::Array; }
    if (i >= arr_.size()) arr_.resize(i + 1);
    return arr_[i];
}

void JsonValue::push(JsonValue v) {
    if (type_ != Type::Array) { type_ = Type::Array; arr_.clear(); }
    arr_.push_back(std::move(v));
}

bool JsonValue::has(std::string_view key) const noexcept { return find(key) != nullptr; }

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) return nullptr;
    for (const auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

JsonValue* JsonValue::find(std::string_view key) noexcept {
    if (type_ != Type::Object) return nullptr;
    for (auto& kv : obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

JsonValue& JsonValue::operator[](std::string_view key) {
    if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
    for (auto& kv : obj_)
        if (kv.first == key) return kv.second;
    obj_.emplace_back(std::string(key), JsonValue());
    return obj_.back().second;
}

void JsonValue::set(std::string_view key, JsonValue v) { (*this)[key] = std::move(v); }

bool JsonValue::getBool(std::string_view key, bool def) const noexcept {
    const JsonValue* v = find(key);
    return v ? v->asBool(def) : def;
}

double JsonValue::getDouble(std::string_view key, double def) const noexcept {
    const JsonValue* v = find(key);
    return v ? v->asDouble(def) : def;
}

int JsonValue::getInt(std::string_view key, int def) const noexcept {
    const JsonValue* v = find(key);
    return v ? v->asInt(def) : def;
}

std::int64_t JsonValue::getInt64(std::string_view key, std::int64_t def) const noexcept {
    const JsonValue* v = find(key);
    return v ? v->asInt64(def) : def;
}

std::string JsonValue::getString(std::string_view key, std::string_view def) const {
    const JsonValue* v = find(key);
    return v ? v->asString(def) : std::string(def);
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

std::string JsonValue::dump(int indent) const {
    std::string out;
    out.reserve(1024);
    dumpTo(out, indent, 0);
    return out;
}

void JsonValue::dumpTo(std::string& out, int indent, int depth) const {
    const bool pretty = indent >= 0;
    const auto newlineIndent = [&](int d) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<std::size_t>(indent * d), ' ');
    };

    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: appendNumber(out, num_); break;
        case Type::String: appendEscaped(out, str_); break;

        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }

            // Arrays of plain numbers (waveform peaks, snapshot vectors) stay on
            // one line; exploding them vertically makes patches unreadable.
            bool allScalar = true;
            for (const auto& e : arr_)
                if (e.isArray() || e.isObject()) { allScalar = false; break; }

            out += '[';
            for (std::size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += allScalar ? ", " : ",";
                if (!allScalar) newlineIndent(depth + 1);
                arr_[i].dumpTo(out, indent, depth + 1);
            }
            if (!allScalar) newlineIndent(depth);
            out += ']';
            break;
        }

        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (std::size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                newlineIndent(depth + 1);
                appendEscaped(out, obj_[i].first);
                out += pretty ? ": " : ":";
                obj_[i].second.dumpTo(out, indent, depth + 1);
            }
            newlineIndent(depth);
            out += '}';
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

JsonValue JsonValue::parse(std::string_view text, std::string* error) {
    // Skip a UTF-8 BOM if an editor left one behind.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    Parser parser(text);
    JsonValue result;
    if (!parser.parseValue(result)) {
        if (error) *error = parser.error();
        return JsonValue();
    }
    if (!parser.atEndAfterWhitespace()) {
        if (error) *error = "trailing content after the top-level value";
        return JsonValue();
    }
    if (error) error->clear();
    return result;
}

} // namespace acm
