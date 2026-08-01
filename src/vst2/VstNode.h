// A hosted VST2 plugin as a node on the patcher canvas.
//
// The plugin's parameters are mirrored as ordinary acomposter Parameters, which
// is what lets the metasurface interpolate them exactly like anything else in
// the patch: a snapshot of a reverb's decay is no different from a snapshot of a
// crossfader position.
//
// The node stays alive and passes audio through even when the plugin has failed
// or its bridge has died, so a crashed plugin mutes one box rather than the set.
#pragma once

#include "../core/Node.h"
#include "PluginManager.h"
#include "Vst2Plugin.h"

#include <memory>
#include <string>
#include <vector>

namespace acm::vst2 {

class VstNode : public Node {
public:
    // The description fixes the port layout, so it is needed at construction.
    explicit VstNode(const PluginDescription& description);
    ~VstNode() override;

    static const char* kTypeName;

    // Instantiates the plugin. Safe to call only before the node joins a graph.
    bool loadPlugin(const PluginManager& manager, double sampleRate, int blockSize,
                    bool forceBridge, std::string* error);

    // -- state for the UI --------------------------------------------------
    const PluginDescription& pluginDescription() const noexcept { return description_; }
    Vst2Plugin* plugin() noexcept { return plugin_.get(); }
    const Vst2Plugin* plugin() const noexcept { return plugin_.get(); }
    bool pluginLoaded() const noexcept { return plugin_ && plugin_->valid(); }
    bool bridged() const noexcept { return bridged_; }

    bool editorOpen() const;
    void toggleEditor();
    void closeEditor();

    // Rebuilds the bridge after a crash, restoring state where possible.
    bool reloadPlugin();
    // Re-reads every parameter out of the plugin. Needed after restoring state
    // behind the node's back, as copying a rack does.
    void refreshParametersFromPlugin() { pullParametersFromPlugin(); }

    // -- programs ----------------------------------------------------------
    int programCount() const;
    int currentProgram() const;
    void setCurrentProgram(int index);
    std::string programName(int index) const;

    // -- midi --------------------------------------------------------------
    void sendMidi(unsigned char status, unsigned char data1, unsigned char data2);
    void allNotesOff();

    // -- Node --------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;
    void serviceFromMessageThread() override;
    int latencyFrames() const override;

    void saveExtraState(JsonValue& out) const override;
    void loadExtraState(const JsonValue& in) override;

    // Set by the app so a node reconstructed from a patch can find its plugin.
    static void setPluginManager(PluginManager* manager) { manager_ = manager; }
    static PluginManager* pluginManager() noexcept { return manager_; }

    // Builds a node from the "state" object a patch saved. Returns null when the
    // plugin is not installed, which the loader turns into a warning.
    static std::unique_ptr<Node> createFromPatchState(const JsonValue& state,
                                                      double sampleRate, int blockSize,
                                                      std::string* error);

private:
    void mirrorParameters();
    void pushChangedParameters();
    void pullParametersFromPlugin();

    static PluginManager* manager_;

    PluginDescription description_;
    Vst2PluginPtr plugin_;
    bool bridged_ = false;
    bool active_ = false;

    ParamIndex pDryWet_ = -1;
    ParamIndex pOutputGain_ = -1;
    // The plugin's own parameters start here, in plugin index order.
    ParamIndex firstPluginParam_ = -1;

    SmoothedValue dryWet_;
    SmoothedValue outputGain_;

    // Last value pushed to the plugin, so only genuine changes cross the
    // boundary. That matters most for bridged plugins, where every change is
    // packed into the audio block's round trip.
    std::vector<float> lastPushed_;

    // Channel pointer scratch for the plugin's own layout.
    std::vector<const float*> inputPointers_;
    std::vector<float*> outputPointers_;
    std::vector<float> wetStorage_;
    std::vector<float*> wetPointers_;

    // State captured at load time, applied once the plugin is instantiated.
    std::vector<std::uint8_t> pendingState_;
    std::string pendingIdentifier_;
    std::string pendingPath_;
    bool pendingForceBridge_ = false;
};

// Registers the loader that lets the patch format reconstruct plugin nodes.
// Called once at start-up from the Windows side; the portable build never sees
// it, which is why VstNode does not live in the node library.
void registerVstNodeLoader(double sampleRate, int blockSize);

} // namespace acm::vst2
