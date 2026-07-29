// The application: owns the engine, the window, the renderer and every view,
// and runs the frame loop that connects them.
//
// Two threads matter here. The audio device calls straight into the engine on
// its own high-priority thread and never touches anything in this class. Every
// other thing - drawing, patch loading, plugin scanning, editor windows - runs
// on this thread. Where the two must meet, they meet through the lock-free
// mechanisms in core: parameter atomics, the graph's published schedule, and the
// block counter that makes deferred deletion safe.
#pragma once

#include "../core/Engine.h"
#include "../gfx/FontAtlas.h"
#include "../gfx/Renderer.h"
#include "../meta/Metasurface.h"
#include "../patch/Patch.h"
#include "../platform/WasapiDevice.h"
#include "../platform/Window.h"
#include "../ui/MetasurfaceView.h"
#include "../ui/Panels.h"
#include "../ui/PatcherView.h"
#include "../ui/Ui.h"
#include "../vst2/PluginManager.h"

#include <memory>
#include <string>

namespace acm::app {

class Application {
public:
    Application();
    ~Application();

    // Returns false and leaves errorText() set when start-up fails.
    bool initialise();
    void shutdown();

    // Runs until the window closes. Returns the process exit code.
    int run();

    const std::string& errorText() const noexcept { return error_; }

private:
    // -- frame -------------------------------------------------------------
    void frame(float deltaSeconds);
    void layout(float deltaSeconds);
    void handleGlobalShortcuts();
    void serviceBackground();

    // -- patch management --------------------------------------------------
    void newPatch();
    void openPatch();
    void openPatchFile(const std::string& path);
    bool savePatch();
    bool savePatchAs();
    bool confirmDiscardChanges();
    void markModified() { modified_ = true; }
    void updateWindowTitle();

    // -- settings ----------------------------------------------------------
    void loadSettings();
    void saveSettings() const;

    // -- audio -------------------------------------------------------------
    bool openAudioDevice();

    // -- plugins -----------------------------------------------------------
    void addPluginNode(const vst2::PluginDescription& description, bool forceBridge);
    void togglePluginEditor(NodeId node);
    void closeAllPluginEditors();

    std::string error_;

    // -- owned subsystems --------------------------------------------------
    Engine engine_;
    Metasurface metasurface_;
    vst2::PluginManager plugins_;

    platform::Window window_;
    platform::WasapiDevice device_;
    platform::AudioDeviceSettings deviceSettings_;

    gfx::Renderer renderer_;
    gfx::FontAtlas fontAtlas_;
    gfx::TextureId fontTexture_ = gfx::kNoTexture;

    ui::Ui ui_;
    ui::InputState input_;

    ui::PatcherView patcher_;
    ui::MetasurfaceView metasurfaceView_;
    ui::BrowserView browser_;
    ui::InspectorView inspector_;
    ui::PluginManagerView pluginView_;
    ui::TransportBar transportBar_;
    ui::StatusBar statusBar_;

    ui::MainView activeView_ = ui::MainView::Patch;

    // -- document state ----------------------------------------------------
    PatchViewState patchView_;
    PatchMetadata patchMetadata_;
    std::string patchPath_;
    bool modified_ = false;

    // -- panel sizes -------------------------------------------------------
    float browserWidth_ = 230.0f;
    float inspectorWidth_ = 280.0f;
    bool showBrowser_ = true;
    bool showInspector_ = true;

    bool vsync_ = true;
    bool initialised_ = false;
};

} // namespace acm::app
