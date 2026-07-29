// A small, dependency-free JSON value / parser / writer.
//
// acomposter ships with no third-party libraries, so the patch format needs its
// own reader and writer. Objects keep insertion order rather than sorting keys,
// which makes saved patches diff-friendly and readable - patches are meant to be
// opened in a text editor and picked apart.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acm {

class JsonValue {
public:
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool v) : type_(Type::Bool), bool_(v) {}
    JsonValue(double v) : type_(Type::Number), num_(v) {}
    JsonValue(float v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    JsonValue(int v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    JsonValue(unsigned v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    JsonValue(std::int64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    JsonValue(std::uint64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    JsonValue(const char* v) : type_(Type::String), str_(v ? v : "") {}
    JsonValue(std::string v) : type_(Type::String), str_(std::move(v)) {}

    static JsonValue array() { JsonValue v; v.type_ = Type::Array; return v; }
    static JsonValue object() { JsonValue v; v.type_ = Type::Object; return v; }

    // -- interrogation -----------------------------------------------------
    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isBool() const noexcept { return type_ == Type::Bool; }
    bool isNumber() const noexcept { return type_ == Type::Number; }
    bool isString() const noexcept { return type_ == Type::String; }
    bool isArray() const noexcept { return type_ == Type::Array; }
    bool isObject() const noexcept { return type_ == Type::Object; }

    // -- scalar access -----------------------------------------------------
    bool asBool(bool def = false) const noexcept;
    double asDouble(double def = 0.0) const noexcept;
    float asFloat(float def = 0.0f) const noexcept { return static_cast<float>(asDouble(def)); }
    int asInt(int def = 0) const noexcept;
    std::int64_t asInt64(std::int64_t def = 0) const noexcept;
    std::string asString(std::string_view def = {}) const;

    // -- array -------------------------------------------------------------
    std::size_t size() const noexcept;
    const JsonValue& at(std::size_t i) const;
    JsonValue& at(std::size_t i);
    void push(JsonValue v);
    const std::vector<JsonValue>& items() const noexcept { return arr_; }

    // -- object ------------------------------------------------------------
    bool has(std::string_view key) const noexcept;
    const JsonValue* find(std::string_view key) const noexcept;
    JsonValue* find(std::string_view key) noexcept;
    // Inserts a null member when the key is absent, so `v["a"]["b"] = 1` works.
    JsonValue& operator[](std::string_view key);
    void set(std::string_view key, JsonValue v);
    const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept { return obj_; }

    // Convenience readers used all over the patch loader; a missing or
    // wrong-typed member yields the fallback instead of throwing.
    bool getBool(std::string_view key, bool def) const noexcept;
    double getDouble(std::string_view key, double def) const noexcept;
    float getFloat(std::string_view key, float def) const noexcept { return static_cast<float>(getDouble(key, def)); }
    int getInt(std::string_view key, int def) const noexcept;
    std::int64_t getInt64(std::string_view key, std::int64_t def) const noexcept;
    std::string getString(std::string_view key, std::string_view def = {}) const;

    // -- serialisation -----------------------------------------------------
    // indent < 0 emits the compact single-line form.
    std::string dump(int indent = 2) const;

    // Returns a Null value and fills `error` on failure.
    static JsonValue parse(std::string_view text, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<JsonValue> arr_;
    std::vector<std::pair<std::string, JsonValue>> obj_;
};

} // namespace acm
