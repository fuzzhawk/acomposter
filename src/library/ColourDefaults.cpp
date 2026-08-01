#include "ColourDefaults.h"

namespace acm::library {
namespace {

// One driven parameter. `param` is matched against the plugin's own control
// names; `red` and `blue` are where that control sits at the two ends, in the
// parameter's own normalised range.
struct Target {
    const char* param;
    float red;
    float neutral;
    float blue;
};

struct Preset {
    const char* name;
    const char* note;
    std::initializer_list<Target> targets;
};

JsonValue toJson(const Preset& preset) {
    JsonValue root = JsonValue::object();
    root.set("format", "acomposter-color-preset");
    root.set("version", 1);
    root.set("name", preset.name);
    root.set("note", preset.note);

    JsonValue array = JsonValue::array();
    for (const Target& target : preset.targets) {
        JsonValue entry = JsonValue::object();
        // No node name and no type: these bind purely by parameter name against
        // whatever is on the rack, which is the only thing that can be known in
        // advance about somebody else's plugins.
        entry.set("param", target.param);
        entry.set("red", target.red);
        entry.set("neutral", target.neutral);
        entry.set("blue", target.blue);
        entry.set("depth", 1.0f);
        entry.set("enabled", true);
        array.push(std::move(entry));
    }

    root.set("targets", std::move(array));
    return root;
}

// Red is tight and dry with the weight left in; blue is open, thin and wet.
// The neutral column is the middle of each control's travel, which is what
// makes the centre of the knob audibly the chain doing nothing.
const Preset kPresets[] = {
    { "bass - tight to washed",
      "low-heavy and dry at red, hipassed and wet at blue",
      { { "Frequency", 0.10f, 0.30f, 0.62f },
        { "Cutoff",    0.10f, 0.30f, 0.62f },
        { "Resonance", 0.20f, 0.30f, 0.55f },
        { "Drive",     0.55f, 0.35f, 0.15f },
        { "Mix",       0.00f, 0.20f, 0.45f },
        { "Wet",       0.00f, 0.20f, 0.45f },
        { "Decay",     0.15f, 0.35f, 0.70f } } },

    { "drums - dry to cavern",
      "close and punchy at red, long room at blue",
      { { "Frequency", 0.15f, 0.35f, 0.70f },
        { "Cutoff",    0.15f, 0.35f, 0.70f },
        { "Mix",       0.00f, 0.22f, 0.60f },
        { "Wet",       0.00f, 0.22f, 0.60f },
        { "Decay",     0.10f, 0.35f, 0.80f },
        { "Size",      0.15f, 0.40f, 0.85f },
        { "Feedback",  0.00f, 0.25f, 0.55f } } },

    { "pads - close to endless",
      "narrow and present at red, wide and drifting at blue",
      { { "Frequency", 0.25f, 0.45f, 0.80f },
        { "Cutoff",    0.25f, 0.45f, 0.80f },
        { "Mix",       0.10f, 0.35f, 0.75f },
        { "Wet",       0.10f, 0.35f, 0.75f },
        { "Size",      0.20f, 0.50f, 0.95f },
        { "Decay",     0.20f, 0.50f, 0.95f },
        { "Width",     0.30f, 0.55f, 1.00f } } },

    { "leads - forward to far",
      "dry and in front at red, delayed and behind at blue",
      { { "Frequency", 0.30f, 0.50f, 0.78f },
        { "Cutoff",    0.30f, 0.50f, 0.78f },
        { "Mix",       0.00f, 0.25f, 0.65f },
        { "Wet",       0.00f, 0.25f, 0.65f },
        { "Feedback",  0.05f, 0.30f, 0.70f },
        { "Delay",     0.10f, 0.35f, 0.65f } } },
};

} // namespace

void seedColourPresets(const PresetStore& store) {
    if (!store.isOpen()) return;

    for (const Preset& preset : kPresets)
        store.saveIfAbsent(preset.name, toJson(preset));
}

} // namespace acm::library
