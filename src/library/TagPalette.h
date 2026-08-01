// The tag palette: the fixed vocabulary a stem set is described with.
//
// A tag is a name and a colour, and that pairing is the whole point. Tagging a
// stem "bass" is what later decides which output it lands on in the stem
// player, which effect rack it inherits, and what colour its marker is on the
// spectral strip. Standardising the vocabulary before import is what makes those
// three things agree without being wired up by hand each time.
//
// The defaults are the categories a bounce usually comes in. They can be
// renamed, recoloured, reordered and added to, because no fixed list survives
// contact with somebody else's session.
#pragma once

#include "../core/Json.h"
#include "../gfx/Geometry.h"

#include <string>
#include <vector>

namespace acm::library {

struct Tag {
    std::string id;      // stable, never shown; what entries actually store
    std::string name;    // shown, editable
    std::uint32_t colour = 0xFF808080u;   // 0xAARRGGBB

    // Which stem player output this tag routes to. -1 means "not routed yet",
    // which is the honest default: a tag is a description first and a routing
    // decision second.
    int outputSlot = -1;

    // The chain preset this category is treated with. Tagging a stem offers to
    // build it, which is the whole point of standardising the vocabulary: the
    // tag is not just a label, it is a decision about how the stem is handled.
    std::string defaultChain;
};

class TagPalette {
public:
    // Fills in the categories a stem bounce usually arrives in.
    void loadDefaults();

    const std::vector<Tag>& tags() const noexcept { return tags_; }
    int count() const noexcept { return static_cast<int>(tags_.size()); }

    const Tag* find(const std::string& id) const;
    int indexOf(const std::string& id) const;

    // Returns the new tag's id. Names need not be unique; ids are made so.
    std::string add(std::string name, std::uint32_t colour);
    void remove(int index);
    void rename(int index, std::string name);
    void setColour(int index, std::uint32_t colour);
    void setOutputSlot(int index, int slot);
    // The saved chain a stem of this kind is offered when it is tagged.
    void setDefaultChain(int index, std::string chainName);
    void move(int from, int to);

    // -- persistence -------------------------------------------------------
    // Written as its own file so it can be edited by hand alongside the rest of
    // the library, and shared between machines without carrying a patch.
    JsonValue toJson() const;
    void fromJson(const JsonValue& value);

    bool save(const std::string& utf8Path) const;
    bool load(const std::string& utf8Path);

private:
    std::string makeUniqueId(const std::string& from) const;

    std::vector<Tag> tags_;
};

} // namespace acm::library
