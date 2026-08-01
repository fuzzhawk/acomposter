// The supporting panels: file browser, node inspector, plugin manager and the
// transport bar.
//
// Each one is a plain object with a render() that draws into a rectangle. They
// hold no state the engine owns - everything they show is read from the graph,
// the metasurface or the plugin manager each frame - so nothing can fall out of
// step with what is actually playing.
#pragma once

#include "../core/Engine.h"
#include "../core/FileIo.h"
#include "../meta/Metasurface.h"
#include "../library/Library.h"
#include "../platform/AudioDevice.h"
#include "../vst2/PluginManager.h"
#include "Ui.h"

#include <functional>
#include <string>
#include <vector>

namespace acm::ui {

// ---------------------------------------------------------------------------
// File browser
// ---------------------------------------------------------------------------

class BrowserView {
public:
    void initialise();

    void render(Ui& ui, const Rect& bounds);

    // Invoked when a patch file is activated.
    std::function<void(const std::string&)> onOpenPatch;
    // Invoked when an audio file is double-clicked, as an alternative to dragging.
    std::function<void(const std::string&)> onLoadSample;

    void navigateTo(const std::string& directory);
    const std::string& currentDirectory() const noexcept { return currentDirectory_; }

private:
    void refresh();
    void drawPlaces(Ui& ui, Rect& area);

    std::string currentDirectory_;
    std::vector<DirectoryEntry> entries_;
    std::string filter_;
    bool needsRefresh_ = true;

    // Edit buffer for the path field, and the directory it was filled from, so
    // typing is not overwritten every frame by the directory we are still in.
    std::string pathBuffer_;
    std::string pathBufferFor_;

    struct Place { std::string name; std::string path; };
    std::vector<Place> places_;
};

// ---------------------------------------------------------------------------
// Node inspector
// ---------------------------------------------------------------------------

class InspectorView {
public:
    void initialise(Engine* engine, Metasurface* metasurface, library::Library* library);

    // `node` is whatever the patcher currently has focused.
    void render(Ui& ui, const Rect& bounds, NodeId node);

    // Raised when the plugin editor button in the inspector is pressed.
    std::function<void(NodeId)> onOpenPluginEditor;
    // Asks the application to put a plugin on the end of a stem's rack. The
    // application owns the plugin list and the wiring, so the inspector only
    // has to say which stem.
    std::function<void(NodeId stemPlayer, int slot)> onAddStemEffect;
    std::function<void(NodeId)> onRemoveFromChain;
    std::function<void(NodeId stemPlayer)> onTidyChains;
    // Copies one stem's rack onto another, plugin state included.
    std::function<void(NodeId stemPlayer, int fromSlot, int toSlot)> onCopyChain;
    // Raised when a stem is given a tag, so the application can offer to build
    // that tag's default rack. Offered rather than done: building a rack loads
    // plugins and takes a moment, and a mistagged file should not silently
    // instantiate four of them.
    std::function<void(NodeId stemPlayer, int slot, const std::string& tagId)> onStemTagged;
    // Copies the stem player's current selection into a build node.
    std::function<void(NodeId stemPlayer, NodeId buildNode)> onSendSnippet;
    // Stores a stem's rack under a name, and puts a stored one back. Both go
    // through the application because both touch the plugin host.
    std::function<void(NodeId stemPlayer, int slot, const std::string& name)> onSaveChain;
    std::function<void(NodeId stemPlayer, int slot, const std::string& name)> onLoadChain;

private:
    void drawParameterList(Ui& ui, Rect area, Node& node);
    void drawPluginSection(Ui& ui, Rect& area, Node& node);
    // The section list is the stem player's real editor: it is the only place
    // sections can be created, so it lives where there is room for it rather
    // than crammed into the node body.
    void drawStemSection(Ui& ui, Rect& area, Node& node);
    void drawStemChains(Ui& ui, Rect& area, Node& node);
    void drawBuildSection(Ui& ui, Rect& area, Node& node);
    void drawDropSection(Ui& ui, Rect& area, Node& node);
    void drawColorSection(Ui& ui, Rect& area, Node& node);

    Engine* engine_ = nullptr;
    Metasurface* metasurface_ = nullptr;
    library::Library* library_ = nullptr;

    std::string nameBuffer_;
    NodeId nameBufferFor_ = kInvalidNode;
    std::string commentBuffer_;
    NodeId commentBufferFor_ = kInvalidNode;

    // Which section row is open for editing, and its edit buffer.
    int editingSection_ = -1;
    int expandedStem_ = -1;
    // Source slot armed by 'copy', waiting for a destination.
    int copySourceStem_ = -1;
    std::string sectionNameBuffer_;

    // The name being typed for a rack about to be saved, and which stem it
    // belongs to. Held per-stem rather than globally so opening a second rack
    // while typing does not silently retarget the save.
    int savingStem_ = -1;
    std::string chainNameBuffer_;
};

// ---------------------------------------------------------------------------
// Plugin manager
// ---------------------------------------------------------------------------

class PluginManagerView {
public:
    void initialise(vst2::PluginManager* manager);

    void render(Ui& ui, const Rect& bounds);

    // Asks the application to instantiate this plugin and drop it on the canvas.
    std::function<void(const vst2::PluginDescription&, bool forceBridge)> onAddPlugin;
    // Asks the application to open a folder picker and return the chosen path.
    std::function<std::string()> onBrowseForFolder;

private:
    vst2::PluginManager* manager_ = nullptr;
    std::string search_;
    bool showFailures_ = false;
    bool forceBridge_ = false;
    // Held by path rather than by row index: the visible list is rebuilt from
    // the search filter every frame, so an index would silently come to mean a
    // different plugin as soon as the filter changed.
    std::string selectedPath_;
};

// ---------------------------------------------------------------------------
// Settings
//
// A modal panel rather than a separate OS window: the whole interface is
// immediate mode, so an in-app sheet needs no second HWND, no second render
// target and no second message loop, and it cannot end up behind the main
// window on a multi-monitor rig.
// ---------------------------------------------------------------------------

class SettingsView {
public:
    void initialise(Engine* engine, platform::AudioDeviceSettings* settings);

    void open();
    // Stops the identify tone on the way out: a blip walking the outputs after
    // the panel has gone is a noise with no visible cause.
    void close();
    bool visible() const noexcept { return visible_; }

    // Draws over `bounds` when open. Returns true if it consumed the frame's
    // input, so the views underneath know to stay still.
    bool render(Ui& ui, const Rect& bounds, const platform::AudioDeviceStatus& status);

    // Raised when the user applies a change that needs the device reopening.
    std::function<void()> onApplyAudioSettings;
    // Opens the ASIO driver's own settings window, which is the only place its
    // buffer size and clock source can be changed.
    std::function<void()> onShowControlPanel;

    // Told by the application whether the device that is actually open has a
    // control panel to show.
    void setControlPanelAvailable(bool available) noexcept {
        controlPanelAvailable_ = available;
    }

private:
    void refreshDeviceLists();

    Engine* engine_ = nullptr;
    platform::AudioDeviceSettings* settings_ = nullptr;

    bool visible_ = false;
    bool deviceListsLoaded_ = false;
    // Set by open(), cleared after the first frame: see the dismissal check.
    bool justOpened_ = false;
    bool asioAvailable_ = false;
    bool controlPanelAvailable_ = false;

    std::vector<platform::AudioDeviceInfo> outputDevices_;
    std::vector<platform::AudioDeviceInfo> inputDevices_;
    std::vector<std::string> outputNames_;
    std::vector<std::string> inputNames_;

    // Working copy; only written back to `settings_` when Apply is pressed, so
    // a half-made choice never restarts the audio device mid-set.
    platform::AudioDeviceSettings draft_;
    bool dirty_ = false;
};

// ---------------------------------------------------------------------------
// Transport bar
// ---------------------------------------------------------------------------

// Which of the main views is on screen.
//
// The first four are *library* tabs: they describe material that outlives any
// one patch, and their state is deliberately not part of the document. Patch and
// Control are the document; Plugins is a tool. Keeping that split explicit here
// is what stops a song's notes being lost because a patch was closed.
enum class MainView : int {
    Projects = 0, Songs, Library, Stems, Patch, Control, Plugins, Count
};

const char* toString(MainView view) noexcept;
// True for the tabs whose state survives a patch being loaded or closed.
bool isLibraryView(MainView view) noexcept;

class TransportBar {
public:
    void initialise(Engine* engine);

    // Returns the height it wants.
    static float height();
    void render(Ui& ui, const Rect& bounds, MainView& activeView);

    std::function<void()> onNewPatch;
    std::function<void()> onOpenPatch;
    std::function<void()> onSavePatch;
    std::function<void()> onSavePatchAs;
    std::function<void()> onOpenSettings;
    // Drawn in the bar so the panel can be reached without a menu bar.
    bool showSettingsButton = true;

    void setPatchName(std::string name) { patchName_ = std::move(name); }
    void setModified(bool modified) { modified_ = modified; }

private:
    Engine* engine_ = nullptr;
    std::string patchName_ = "untitled";
    bool modified_ = false;

    std::string bpmBuffer_;
    bool editingBpm_ = false;
    // Wall-clock seconds, fed to the transport's tap-tempo averager.
    double tapClock_ = 0.0;
};

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

class StatusBar {
public:
    void initialise(Engine* engine, vst2::PluginManager* plugins);

    static float height();
    void render(Ui& ui, const Rect& bounds, const std::string& deviceDescription);

private:
    Engine* engine_ = nullptr;
    vst2::PluginManager* plugins_ = nullptr;
};

} // namespace acm::ui
