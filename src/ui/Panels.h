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

    struct Place { std::string name; std::string path; };
    std::vector<Place> places_;
};

// ---------------------------------------------------------------------------
// Node inspector
// ---------------------------------------------------------------------------

class InspectorView {
public:
    void initialise(Engine* engine, Metasurface* metasurface);

    // `node` is whatever the patcher currently has focused.
    void render(Ui& ui, const Rect& bounds, NodeId node);

    // Raised when the plugin editor button in the inspector is pressed.
    std::function<void(NodeId)> onOpenPluginEditor;

private:
    void drawParameterList(Ui& ui, Rect area, Node& node);
    void drawPluginSection(Ui& ui, Rect& area, Node& node);

    Engine* engine_ = nullptr;
    Metasurface* metasurface_ = nullptr;

    std::string nameBuffer_;
    NodeId nameBufferFor_ = kInvalidNode;
    std::string commentBuffer_;
    NodeId commentBufferFor_ = kInvalidNode;
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
    int selectedIndex_ = -1;
};

// ---------------------------------------------------------------------------
// Transport bar
// ---------------------------------------------------------------------------

// Which of the main views is on screen.
enum class MainView : int { Patch = 0, Metasurface, Plugins };

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
