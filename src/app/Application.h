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
#include "../platform/AudioDevice.h"
#include "../platform/Window.h"
#include "../ui/MetasurfaceView.h"
#include "../ui/Panels.h"
#include "../ui/PatcherView.h"
#include "../ui/StemBrowserView.h"
#include "../library/Library.h"
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

    // The rectangle the patcher canvas occupies, matching exactly what layout()
    // hands to PatcherView::render. Anything that converts screen coordinates to
    // world coordinates outside of the render pass has to go through here, or it
    // works in a different coordinate space to the one on screen.
    gfx::Rect canvasBounds() const;

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
    // Closes and reopens the device with the current settings, keeping the patch.
    void restartAudioDevice();

    // -- plugins -----------------------------------------------------------
    void addPluginNode(const vst2::PluginDescription& description, bool forceBridge);
    // Opens the plugin list so the next choice lands on a stem's rack rather
    // than on the canvas.
    void beginStemEffectPick(NodeId stemPlayer, int slot);
    void togglePluginEditor(NodeId node);
    void closeAllPluginEditors();

    std::string error_;

    // -- owned subsystems --------------------------------------------------
    Engine engine_;
    Metasurface metasurface_;
    vst2::PluginManager plugins_;

    platform::Window window_;
    // Which backend is behind this is a setting, so it is held by pointer and
    // replaced wholesale when the setting changes. Never null once initialised.
    std::unique_ptr<platform::AudioDevice> device_;
    // What `device_` actually is, which is not always what the settings ask for:
    // a failed ASIO open falls back to WASAPI.
    platform::AudioBackend deviceBackend_ = platform::AudioBackend::Wasapi;
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
    ui::StemBrowserView stemBrowser_;
    ui::SettingsView settings_;
    ui::TransportBar transportBar_;
    ui::StatusBar statusBar_;

    ui::MainView activeView_ = ui::MainView::Patch;

    // The library outlives any patch: songs, projects, tagged assets and the
    // tag palette are opened once at start-up and are deliberately untouched by
    // loading or closing a document.
    library::Library library_;
    void drawLibraryPlaceholder(const gfx::Rect& bounds, ui::MainView view);

    // When set, the next plugin chosen in the manager is inserted into this
    // stem's rack instead of being dropped on the canvas.
    NodeId pendingChainPlayer_ = kInvalidNode;
    int pendingChainSlot_ = -1;

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

    // Reserved strip along the bottom of the patch view for the arrangement
    // timeline. Nothing draws into it yet beyond its own ruler; it is laid out
    // now so that the canvas, the drop hit-testing and the saved layout all
    // already account for it, and turning it on later is not a change to any of
    // them.
    float timelineHeight_ = 150.0f;
    bool showTimeline_ = false;
    void drawTimelinePlaceholder(const gfx::Rect& bounds);

    bool vsync_ = true;
    bool initialised_ = false;
};

} // namespace acm::app
