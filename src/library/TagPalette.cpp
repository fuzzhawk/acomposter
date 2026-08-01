#include "TagPalette.h"

#include "../core/FileIo.h"

#include <algorithm>
#include <cctype>

namespace acm::library {
namespace {

std::string slugify(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out.push_back(static_cast<char>(std::tolower(u)));
        else if (!out.empty() && out.back() != '-') out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string("tag") : out;
}

} // namespace

void TagPalette::loadDefaults() {
    tags_.clear();

    // Ordered the way a mix is usually built rather than alphabetically, so the
    // palette reads as a session rather than as a list.
    // Output slots are assigned here rather than left unset, so a freshly
    // tagged folder of stems is already routed. Kick and snare share the drum
    // bus by default because most sessions want them together until they do
    // not, and moving one is a single click.
    const struct { const char* name; std::uint32_t colour; int output; } defaults[] = {
        { "drums mixed",      0xFFE0533Cu, 0 },
        { "kick",             0xFFC43A2Eu, 0 },
        { "snare",            0xFFE07A3Cu, 0 },
        { "percussion",       0xFFE0A93Cu, 6 },
        { "bass",             0xFF9B5DE5u, 1 },
        { "synth leads",      0xFF3CC8E0u, 2 },
        { "pads",             0xFF3C7AE0u, 3 },
        { "vocals",           0xFF4FD98Au, 5 },
        { "fx",               0xFF8A8F98u, 6 },
    };

    for (const auto& entry : defaults) {
        const std::string id = add(entry.name, entry.colour);
        setOutputSlot(indexOf(id), entry.output);
    }
}

const Tag* TagPalette::find(const std::string& id) const {
    const int index = indexOf(id);
    return index >= 0 ? &tags_[static_cast<std::size_t>(index)] : nullptr;
}

int TagPalette::indexOf(const std::string& id) const {
    for (std::size_t i = 0; i < tags_.size(); ++i)
        if (tags_[i].id == id) return static_cast<int>(i);
    return -1;
}

std::string TagPalette::makeUniqueId(const std::string& from) const {
    const std::string base = slugify(from);
    if (indexOf(base) < 0) return base;

    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string candidate = base + "-" + std::to_string(suffix);
        if (indexOf(candidate) < 0) return candidate;
    }
    return base + "-x";
}

std::string TagPalette::add(std::string name, std::uint32_t colour) {
    Tag tag;
    tag.id = makeUniqueId(name);
    tag.name = std::move(name);
    tag.colour = colour;
    tags_.push_back(tag);
    return tag.id;
}

void TagPalette::remove(int index) {
    if (index < 0 || index >= count()) return;
    tags_.erase(tags_.begin() + index);
}

void TagPalette::rename(int index, std::string name) {
    // The id deliberately does not follow the name: entries reference it, and
    // renaming a tag must not orphan everything already tagged with it.
    if (index < 0 || index >= count()) return;
    tags_[static_cast<std::size_t>(index)].name = std::move(name);
}

void TagPalette::setColour(int index, std::uint32_t colour) {
    if (index < 0 || index >= count()) return;
    tags_[static_cast<std::size_t>(index)].colour = colour;
}

void TagPalette::setOutputSlot(int index, int slot) {
    if (index < 0 || index >= count()) return;
    tags_[static_cast<std::size_t>(index)].outputSlot = slot;
}

void TagPalette::setDefaultChain(int index, std::string chainName) {
    if (index < 0 || index >= count()) return;
    tags_[static_cast<std::size_t>(index)].defaultChain = std::move(chainName);
}

void TagPalette::move(int from, int to) {
    if (from < 0 || from >= count() || to < 0 || to >= count() || from == to) return;
    Tag tag = tags_[static_cast<std::size_t>(from)];
    tags_.erase(tags_.begin() + from);
    tags_.insert(tags_.begin() + to, std::move(tag));
}

JsonValue TagPalette::toJson() const {
    JsonValue root = JsonValue::object();
    root.set("format", "acomposter-tags");
    root.set("version", 1);

    JsonValue array = JsonValue::array();
    for (const Tag& tag : tags_) {
        JsonValue entry = JsonValue::object();
        entry.set("id", tag.id);
        entry.set("name", tag.name);
        // Written as hex text rather than a number: this file is meant to be
        // opened in an editor, and 0xFF3CC8E0 says "colour" where 4281505504
        // says nothing.
        char colour[16];
        std::snprintf(colour, sizeof(colour), "0x%08X", tag.colour);
        entry.set("colour", colour);
        entry.set("outputSlot", tag.outputSlot);
        if (!tag.defaultChain.empty()) entry.set("defaultChain", tag.defaultChain);
        array.push(entry);
    }
    root.set("tags", array);
    return root;
}

void TagPalette::fromJson(const JsonValue& value) {
    const JsonValue* array = value.find("tags");
    if (!array || !array->isArray()) return;

    tags_.clear();
    for (std::size_t i = 0; i < array->size(); ++i) {
        const JsonValue& entry = array->at(i);

        Tag tag;
        tag.name = entry.getString("name", "tag");
        tag.id = entry.getString("id");
        if (tag.id.empty()) tag.id = makeUniqueId(tag.name);

        const std::string colour = entry.getString("colour");
        tag.colour = colour.empty()
            ? 0xFF808080u
            : static_cast<std::uint32_t>(std::strtoul(colour.c_str(), nullptr, 0));

        tag.outputSlot = entry.getInt("outputSlot", -1);
        tag.defaultChain = entry.getString("defaultChain");
        tags_.push_back(std::move(tag));
    }
}

bool TagPalette::save(const std::string& utf8Path) const {
    return writeFileText(utf8Path, toJson().dump(2));
}

bool TagPalette::load(const std::string& utf8Path) {
    std::string text;
    if (!readFileText(utf8Path, text)) return false;

    std::string error;
    const JsonValue root = JsonValue::parse(text, &error);
    if (!error.empty() || !root.isObject()) return false;

    fromJson(root);
    return !tags_.empty();
}

} // namespace acm::library
