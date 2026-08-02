#include "Application.h"

#include "../core/AppPaths.h"
#include "../core/FileIo.h"
#include "../core/Json.h"
#include "../nodes/NodeFactory.h"
#include "../nodes/SamplePlayerNode.h"
#include "../nodes/BuildNode.h"
#include "../nodes/ColorNode.h"
#include "../nodes/StemPlayerNode.h"
#include "../platform/DragOut.h"
#include "../platform/FileDialog.h"
#include "../vst2/BridgedVst2Plugin.h"
#include "../vst2/NativeVst2Plugin.h"
#include "../vst2/VstNode.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <windows.h>

namespace acm::app {

using gfx::Rect;
using gfx::Vec2;

namespace {

constexpr int kDefaultWindowWidth = 1500;
constexpr int kDefaultWindowHeight = 940;

const std::vector<platform::FileFilter>& patchFilters() {
    static const std::vector<platform::FileFilter> filters = {
        { "acomposter patch", "*.acp" },
    };
    return filters;
}

} // namespace

Application::Application() = default;

Application::~Application() { shutdown(); }

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

bool Application::initialise() {
    paths::ensureDirectories();
    registerBuiltinNodes();

    if (!window_.create("acomposter", kDefaultWindowWidth, kDefaultWindowHeight)) {
        error_ = "could not create the application window";
        return false;
    }

    if (!renderer_.initialise(window_.handle(), window_.width(), window_.height())) {
        error_ = renderer_.errorText();
        return false;
    }

    // Fonts are rasterised at the window's DPI, so the interface is sharp on a
    // high-density laptop panel without being unreadable on a projector.
    ui::registerThemeFonts(fontAtlas_, window_.dpiScale());
    if (!fontAtlas_.build()) {
        error_ = "could not rasterise the interface fonts";
        return false;
    }

    fontTexture_ = renderer_.uploadFontAtlas(fontAtlas_);
    fontAtlas_.releasePixels();

    ui_.setFontAtlas(&fontAtlas_);
    ui_.draw().setDefaultTexture(fontTexture_);
    ui_.draw().setWhitePixelUv(fontAtlas_.whitePixelUv());
    ui_.overlay().setDefaultTexture(fontTexture_);
    ui_.overlay().setWhitePixelUv(fontAtlas_.whitePixelUv());

    // -- views -------------------------------------------------------------
    patcher_.initialise(&engine_, &metasurface_, &plugins_);
    metasurfaceView_.initialise(&engine_, &metasurface_, &renderer_);
    controlServer_.initialise(&engine_, &surface_);
    controlView_.initialise(&engine_, &surface_, &metasurfaceView_, &controlServer_);
    controlView_.onModified = [this] { markModified(); };
    browser_.initialise();

    // The library is opened before any patch and is never touched by one.
    int skipped = 0;
    library_.open(pathJoin(paths::documents(), "acomposter\\library"), &skipped);
    if (skipped > 0) {
        ui_.notify(std::to_string(skipped) + " library entries could not be read",
                   ui::theme().danger, 6.0f);
    }

    stemBrowser_.initialise(&engine_, &library_);
    stemBrowser_.navigateTo(paths::musicFolder().empty() ? paths::documents()
                                                         : paths::musicFolder());
    stemBrowser_.onSendToPatch = [this](const std::string& path, const std::string&) {
        auto player = std::make_unique<SamplePlayerNode>();
        player->loadFile(path, nullptr);
        patcher_.placeNode(std::move(player), patcher_.defaultDropPosition(canvasBounds()));
        activeView_ = ui::MainView::Patch;
        markModified();
    };
    songsView_.initialise(&engine_, &library_, library::EntryKind::Song);
    projectsView_.initialise(&engine_, &library_, library::EntryKind::Project);

    const auto openOnCanvas = [this](const std::string& path) {
        auto player = std::make_unique<SamplePlayerNode>();
        player->loadFile(path, nullptr);
        patcher_.placeNode(std::move(player), patcher_.defaultDropPosition(canvasBounds()));
        activeView_ = ui::MainView::Patch;
        markModified();
    };
    songsView_.onSendToPatch = openOnCanvas;
    projectsView_.onSendToPatch = openOnCanvas;

    librarianView_.initialise(&engine_, &library_);
    librarianView_.onSendToPatch = openOnCanvas;
    librarianView_.onBrowseForFolder = [this]() -> std::string {
        return platform::pickFolderDialog(window_.handle(), "Choose a folder of samples");
    };
    // Starts somewhere rather than at "no folder chosen", which is a dead end
    // for anyone who has not yet learned there is a folder button.
    librarianView_.openFolder(paths::musicFolder().empty() ? paths::documents()
                                                           : paths::musicFolder());

    inspector_.initialise(&engine_, &metasurface_, &library_);
    pluginView_.initialise(&plugins_);
    settings_.initialise(&engine_, &deviceSettings_);
    settings_.onApplyAudioSettings = [this] { restartAudioDevice(); };
    settings_.onShowControlPanel = [this] { if (device_) device_->showControlPanel(); };
    transportBar_.initialise(&engine_);
    statusBar_.initialise(&engine_, &plugins_);

    browser_.onOpenPatch = [this](const std::string& path) { openPatchFile(path); };
    browser_.onLoadSample = [this](const std::string& path) {
        auto player = std::make_unique<SamplePlayerNode>();
        player->loadFile(path, nullptr);
        patcher_.placeNode(std::move(player), patcher_.defaultDropPosition(canvasBounds()));
        markModified();
    };

    // The canvas's port menu does the same three things the inspector does, so
    // it is given the same handlers rather than a second implementation of
    // them. Assigned after the inspector's, which is where they are defined.
    patcher_.chainNames = [this] {
        return library_.chains().isOpen() ? library_.chains().names()
                                          : std::vector<std::string>{};
    };

    inspector_.onOpenPluginEditor = [this](NodeId node) { togglePluginEditor(node); };
    inspector_.onAddStemEffect = [this](NodeId player, int slot) {
        beginStemEffectPick(player, slot);
    };
    inspector_.onRemoveFromChain = [this](NodeId node) {
        if (patcher_.removeFromChain(node)) markModified();
    };
    inspector_.onCopyChain = [this](NodeId player, int from, int to) {
        const int copied = patcher_.copyStemChain(player, from, to);
        if (copied > 0) {
            markModified();
            ui_.notify("copied " + std::to_string(copied) + " effect"
                           + (copied == 1 ? "" : "s"), ui::theme().accent, 2.5f);
        } else {
            ui_.notify("nothing to copy from that stem", ui::theme().danger, 3.0f);
        }
    };
    inspector_.onSendSnippet = [this](NodeId playerId, NodeId buildId) {
        auto* stems = dynamic_cast<StemPlayerNode*>(engine_.graph().node(playerId));
        auto* build = dynamic_cast<BuildNode*>(engine_.graph().node(buildId));
        if (!stems || !build) return;

        const int beatsPerBar = engine_.transport().snapshot().timeSigNumerator;
        auto snippet = stems->extractSnippet(beatsPerBar);
        if (!snippet) {
            ui_.notify("that selection is too short to use", ui::theme().danger, 4.0f);
            return;
        }

        char label[96];
        std::snprintf(label, sizeof(label), "%s %.2fs",
                      stems->stemName(stems->snippet().slot).c_str(),
                      snippet->durationSeconds());

        build->setSnippet(std::move(snippet), label);
        markModified();
        ui_.notify("snippet sent to " + build->name(), ui::theme().accent, 2.5f);
    };
    patcher_.onSaveChain = [this](NodeId player, int slot, const std::string& name) {
        if (inspector_.onSaveChain) inspector_.onSaveChain(player, slot, name);
    };
    patcher_.onLoadChain = [this](NodeId player, int slot, const std::string& name) {
        if (inspector_.onLoadChain) inspector_.onLoadChain(player, slot, name);
    };
    patcher_.onCopyChain = [this](NodeId player, int from, int to) {
        if (inspector_.onCopyChain) inspector_.onCopyChain(player, from, to);
    };

    inspector_.onTidyChains = [this](NodeId player) {
        patcher_.tidyStemChains(player);
        markModified();
    };
    inspector_.onStemTagged = [this](NodeId player, int slot, const std::string& tagId) {
        // Tagging a stem is a statement about what it is, and the tag may know
        // how that kind of stem gets treated. Offered rather than done: building
        // a rack loads plugins and takes a moment, and a mistagged stem should
        // not silently instantiate four of them over the one already there.
        const library::Tag* tag = library_.palette().find(tagId);
        if (!tag || tag->defaultChain.empty()) return;

        library::ChainPreset preset;
        if (!library_.chains().load(tag->defaultChain, preset) || preset.empty()) return;

        const std::string message =
            "\"" + tag->name + "\" comes with the chain \"" + tag->defaultChain + "\" ("
            + std::to_string(preset.plugins.size()) + " plugins).\n\n"
            "Put it on this stem? Anything already on the stem is replaced.";

        if (!platform::confirmDialog(window_.handle(), "Apply the tag's chain", message)) return;

        std::vector<std::string> missing;
        const int placed = patcher_.applyStemChain(player, slot, preset, &missing);
        markModified();
        ui_.notify("loaded \"" + tag->defaultChain + "\"  ("
                       + std::to_string(placed) + " plugins)",
                   missing.empty() ? ui::theme().accent : ui::theme().warning, 3.0f);
    };
    inspector_.onColourTargetsChanged = [this] {
        reconcileColourExclusions();
        markModified();
    };

    inspector_.onSaveColour = [this](NodeId nodeId, const std::string& name) {
        auto* colour = dynamic_cast<ColorNode*>(engine_.graph().node(nodeId));
        if (!colour || !library_.colours().isOpen()) return;

        if (library_.colours().save(name, colour->savePreset(name, engine_.graph()))) {
            ui_.notify("saved colour \"" + name + "\"  ("
                           + std::to_string(colour->targets().size()) + " targets)",
                       ui::theme().accent, 2.5f);
        } else {
            ui_.notify("could not write that colour preset", ui::theme().danger, 4.0f);
        }
    };
    inspector_.onLoadColour = [this](NodeId nodeId, const std::string& name) {
        auto* colour = dynamic_cast<ColorNode*>(engine_.graph().node(nodeId));
        if (!colour) return;

        JsonValue preset;
        if (!library_.colours().load(name, preset)) {
            ui_.notify("could not read \"" + name + "\"", ui::theme().danger, 4.0f);
            return;
        }

        std::vector<std::string> unmatched;
        const int bound = colour->loadPreset(preset, engine_.graph(), &unmatched);
        reconcileColourExclusions();
        markModified();

        // A preset binds by parameter name against whatever plugins are
        // actually on the rack, so a partial match is the normal case rather
        // than a failure - but it has to be said, or the missing half is only
        // discovered by the sound being wrong.
        if (unmatched.empty()) {
            ui_.notify("bound " + std::to_string(bound) + " targets", ui::theme().accent, 2.5f);
        } else {
            std::string message = std::to_string(bound) + " bound, "
                                + std::to_string(unmatched.size()) + " not found: "
                                + unmatched.front();
            if (unmatched.size() > 1) message += " and others";
            ui_.notify(message, ui::theme().warning, 6.0f);
        }
    };
    inspector_.onSaveChain = [this](NodeId player, int slot, const std::string& name) {
        if (!library_.chains().isOpen()) {
            ui_.notify("no library open to save into", ui::theme().danger, 3.0f);
            return;
        }

        const library::ChainPreset preset = patcher_.captureStemChain(player, slot, name);
        if (preset.empty()) {
            ui_.notify("nothing on that stem to save", ui::theme().danger, 3.0f);
            return;
        }

        if (!library_.chains().save(preset)) {
            ui_.notify("could not write that chain", ui::theme().danger, 4.0f);
            return;
        }

        ui_.notify("saved \"" + name + "\"  ("
                       + std::to_string(preset.plugins.size()) + " plugins)",
                   ui::theme().accent, 2.5f);

        // The moment a rack is judged good enough to name is the moment it is
        // worth making it the default for its kind of stem - and it is the only
        // moment the intent is unambiguous, so the offer is made here rather
        // than given a control of its own somewhere in the tag editor.
        auto* stems = dynamic_cast<StemPlayerNode*>(engine_.graph().node(player));
        if (!stems) return;

        const int tagIndex = library_.palette().indexOf(stems->stemTag(slot));
        const library::Tag* tag = library_.palette().find(stems->stemTag(slot));
        if (tagIndex < 0 || !tag || tag->defaultChain == name) return;

        const std::string message =
            "Make \"" + name + "\" the default chain for \"" + tag->name + "\"?\n\n"
            + (tag->defaultChain.empty()
                   ? "Stems tagged this way will offer to load it."
                   : "It replaces \"" + tag->defaultChain + "\".");

        if (!platform::confirmDialog(window_.handle(), "Set the tag's default chain", message))
            return;

        library_.palette().setDefaultChain(tagIndex, name);
        library_.savePalette();
    };
    inspector_.onLoadChain = [this](NodeId player, int slot, const std::string& name) {
        library::ChainPreset preset;
        if (!library_.chains().load(name, preset)) {
            ui_.notify("could not read \"" + name + "\"", ui::theme().danger, 4.0f);
            return;
        }

        std::vector<std::string> missing;
        const int placed = patcher_.applyStemChain(player, slot, preset, &missing);
        markModified();

        // A chain that came back short says which plugins it could not find. The
        // alternative - a quietly shorter rack - is the kind of thing that gets
        // discovered on stage.
        if (missing.empty()) {
            ui_.notify("loaded \"" + name + "\"  (" + std::to_string(placed) + " plugins)",
                       ui::theme().accent, 2.5f);
        } else {
            std::string message = std::to_string(placed) + " loaded, missing: " + missing.front();
            if (missing.size() > 1)
                message += " and " + std::to_string(missing.size() - 1) + " more";
            ui_.notify(message, ui::theme().warning, 5.0f);
        }
    };

    pluginView_.onAddPlugin = [this](const vst2::PluginDescription& description, bool forceBridge) {
        addPluginNode(description, forceBridge);
    };
    pluginView_.onBrowseForFolder = [this]() -> std::string {
        return platform::pickFolderDialog(window_.handle(), "Choose a VST2 plugin folder");
    };

    transportBar_.onNewPatch = [this] { newPatch(); };
    transportBar_.onOpenPatch = [this] { openPatch(); };
    transportBar_.onSavePatch = [this] { savePatch(); };
    transportBar_.onSavePatchAs = [this] { savePatchAs(); };
    transportBar_.onOpenSettings = [this] { settings_.open(); };

    window_.onResize = [this](int width, int height) { renderer_.resize(width, height); };
    window_.onCloseRequested = [this]() { return confirmDiscardChanges(); };

    // -- plugins -----------------------------------------------------------
    vst2::BridgedVst2Plugin::setHelperDirectory(paths::executableDirectory());
    vst2::NativeVst2Plugin::setOwnerWindow(window_.handle());
    vst2::VstNode::setPluginManager(&plugins_);

    if (!plugins_.loadCache(paths::pluginCacheFile()))
        plugins_.useDefaultSearchPaths();

    loadSettings();

    // -- audio -------------------------------------------------------------
    if (!openAudioDevice()) {
        // Not fatal: the patcher is still usable, and the device can be opened
        // once the user has sorted out whatever is wrong.
        ui_.notify("no audio device: " + device_->status().error, ui::theme().danger, 8.0f);
    }

    // The plugin node loader needs the engine's rate, so it is registered after
    // the device has settled on one.
    vst2::registerVstNodeLoader(engine_.stats().sampleRate,
                                std::max(64, engine_.stats().blockSize));

    newPatch();
    initialised_ = true;
    return true;
}

bool Application::openAudioDevice() {
    // The backend is part of the settings, so the object itself is replaced
    // rather than reconfigured. Anything that was running has already been
    // closed by the caller.
    if (!device_ || deviceBackend_ != deviceSettings_.backend) {
        device_ = platform::createAudioDevice(deviceSettings_.backend);
        deviceBackend_ = deviceSettings_.backend;
    }

    const auto callback = [this](const float* input, int inputChannels,
                                 float* output, int outputChannels, int frames) {
        engine_.processInterleaved(input, inputChannels, output, outputChannels, frames);
    };

    bool opened = device_->open(deviceSettings_, callback);

    // ASIO is the better device when it is there, but it is also the one that
    // can be held by another application, uninstalled, or simply unplugged.
    // Falling back keeps the rest of the program usable instead of leaving it
    // silent with an error nobody reads.
    if (!opened && deviceSettings_.backend == platform::AudioBackend::Asio) {
        const std::string reason = device_->status().error;

        device_ = platform::createAudioDevice(platform::AudioBackend::Wasapi);
        deviceBackend_ = platform::AudioBackend::Wasapi;

        platform::AudioDeviceSettings fallback = deviceSettings_;
        fallback.backend = platform::AudioBackend::Wasapi;
        // The device id and channel routing belong to the ASIO driver and mean
        // nothing to an endpoint, so the fallback takes the defaults.
        fallback.outputDeviceId.clear();
        fallback.inputDeviceId.clear();
        fallback.outputChannelCount = 0;
        fallback.outputChannelOffset = 0;

        opened = device_->open(fallback, callback);
        if (opened) {
            ui_.notify("ASIO unavailable (" + reason + ") - using WASAPI",
                       ui::theme().danger, 8.0f);
        }
    }

    const platform::AudioDeviceStatus status = device_->status();

    // The engine is prepared even when the device failed, so the graph still has
    // a sane sample rate for anything the user builds in the meantime.
    engine_.prepare(status.running ? status.sampleRate : 48000.0,
                    status.running ? std::max(status.blockSize, 64) : 512,
                    status.inputChannels,
                    status.running ? std::max(status.outputChannels, 2) : 2);

    return opened;
}

void Application::restartAudioDevice() {
    // Stop first: the callback must not be running while the graph is re-prepared.
    if (device_) device_->close();

    if (openAudioDevice()) {
        ui_.notify("audio device restarted", ui::theme().accent, 2.5f);
    } else {
        const std::string error = device_->status().error;
        ui_.notify("could not open that device: " + (error.empty() ? "unknown error" : error),
                   ui::theme().danger, 8.0f);
    }

    saveSettings();
}

void Application::shutdown() {
    if (!initialised_) return;
    initialised_ = false;

    // Order matters on the way out: stop the audio callback before anything it
    // touches is destroyed, and close plugin editors before their plugins.
    if (device_) device_->close();
    closeAllPluginEditors();

    saveSettings();
    plugins_.saveCache(paths::pluginCacheFile());
    plugins_.cancelScan();

    engine_.graph().clear();
    engine_.graph().collectGarbage();

    metasurfaceView_.shutdown();
    renderer_.shutdown();
    window_.destroy();
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

int Application::run() {
    using Clock = std::chrono::steady_clock;
    auto previous = Clock::now();

    while (window_.pumpEvents(input_)) {
        const auto now = Clock::now();
        // Clamped: a long stall (a modal dialog, a plugin scan) must not make
        // every animation jump when the loop resumes.
        const float deltaSeconds = clampValue(
            std::chrono::duration<float>(now - previous).count(), 0.0f, 0.1f);
        previous = now;

        frame(deltaSeconds);
    }

    return 0;
}

void Application::frame(float deltaSeconds) {
    const Vec2 displaySize{ static_cast<float>(window_.width()),
                            static_cast<float>(window_.height()) };

    // Nothing to draw into: the window is minimised.
    if (displaySize.x < 1.0f || displaySize.y < 1.0f) {
        ::Sleep(16);
        return;
    }

    serviceBackground();

    ui_.beginFrame(input_, displaySize, deltaSeconds);
    metasurfaceView_.update(deltaSeconds);

    layout(deltaSeconds);
    handleGlobalShortcuts();

    // Learn: whatever parameter a widget was moved this frame becomes the
    // control surface's next binding. Done after layout, because the parameter
    // in question is touched somewhere inside it, and the address is resolved
    // by looking the Parameter* back up in the graph - the widgets report a
    // pointer because that is all they have, and only the application knows
    // which node it came from.
    if (controlView_.learning()) {
        if (const Parameter* touched = ui_.touchedParameter()) {
            ParamAddress address;
            for (const auto& node : engine_.graph().nodes()) {
                for (int p = 0; p < node->numParameters(); ++p) {
                    if (&node->parameter(p) != touched) continue;
                    address = ParamAddress{ node->id(), p };
                    break;
                }
                if (address.valid()) break;
            }

            if (address.valid() && controlView_.completeLearn(address))
                ui_.notify("bound", ui::theme().accent, 1.5f);
        }
    }

    // A file dragged out of the window belongs to whatever is under the pointer
    // out there, not to us. The internal drag stays exactly as it was for
    // anything dropped inside; leaving the window is what turns it into a
    // Windows drag, which is also the only gesture that could mean this - the
    // canvas is inside, so a drag that ends inside is still a drag to the
    // canvas.
    //
    // The pointer is captured for the length of a press, so it keeps being
    // reported after it leaves - which is the only reason this can be noticed
    // at all.
    const Vec2 pointer = ui_.input().mousePosition;
    const bool outside = pointer.x < 0.0f || pointer.y < 0.0f
                      || pointer.x > static_cast<float>(window_.width())
                      || pointer.y > static_cast<float>(window_.height());
    if (ui_.dragging() && ui_.dragType() == "file" && outside) {
        const std::string path = ui_.dragPayload();
        ui_.cancelDrag();
        ui_.clearActive();
        platform::dragOutFiles({ path });
    }

    ui_.endFrame();
    window_.applyCursor(ui_.cursor());

    renderer_.beginFrame(ui::theme().background);
    renderer_.render(ui_.draw());
    renderer_.render(ui_.overlay());
    renderer_.endFrame(vsync_);
}

void Application::serviceBackground() {
    // Retire anything the audio thread has finished with, and give every node
    // its chance to do message-thread work (plugin idle, waveform rebuilds).
    engine_.serviceFromMessageThread();

    // Publishes a finished folder scan. Cheap when there is not one.
    librarianView_.serviceFromMessageThread();

    // Applies whatever a tablet moved, and pushes the surface back out. Both
    // happen here so the graph has exactly one writer.
    controlServer_.serviceFromMessageThread();

    // Files dropped on the window go to the canvas. The rectangle has to be the
    // one the patcher actually draws into, or the drop lands at the wrong world
    // position and misses the node it was aimed at.
    if (!input_.droppedFiles.empty()) {
        const Rect canvas = canvasBounds();
        for (const std::string& file : input_.droppedFiles) {
            if (patcher_.handleFileDrop(file, input_.dropPosition, canvas))
                markModified();
        }
    }

    // The palette's tag-to-output map, pushed into every stem player. The node
    // resolves tags to buses itself and never learns what a library is; this is
    // the only place the two meet.
    {
        std::vector<std::pair<std::string, int>> routing;
        for (const library::Tag& tag : library_.palette().tags())
            routing.emplace_back(tag.id, tag.outputSlot);

        for (const auto& node : engine_.graph().nodes()) {
            if (auto* stems = dynamic_cast<StemPlayerNode*>(node.get()))
                stems->setTagRouting(routing);
        }
    }

    // An ASIO driver asking to be reset, because the user changed the buffer
    // size in its control panel or the interface was unplugged and put back.
    // It has to happen here rather than in the callback: reopening the driver
    // from inside its own callback deadlocks it.
    if (device_ && device_->consumeResetRequest()) {
        restartAudioDevice();
        return;
    }

    // A plugin editor opened from the canvas.
    if (const NodeId request = patcher_.consumeEditorRequest(); request != kInvalidNode)
        togglePluginEditor(request);

    // A stem's output port was right-clicked: show its rack. The inspector is
    // opened if it was hidden, because otherwise the gesture would appear to do
    // nothing at all.
    if (const auto rack = patcher_.consumeRackRequest(); rack.valid()) {
        showInspector_ = true;
        inspector_.expandStemRack(rack.slot);
        ui_.notify("stem " + std::to_string(rack.slot + 1) + " rack",
                   ui::theme().accent, 2.0f);
    }
}

void Application::reconcileColourExclusions() {
    // Every parameter any colour node currently drives, whether or not that
    // target is enabled - a disabled target is one the performer means to bring
    // back, and handing it to the surface in the meantime would leave it
    // somewhere unexpected when they do.
    std::unordered_set<std::uint64_t> driven;
    for (const auto& node : engine_.graph().nodes()) {
        const auto* colour = dynamic_cast<const ColorNode*>(node.get());
        if (!colour) continue;
        for (const ColorTarget& target : colour->targets())
            if (target.address.valid()) driven.insert(target.address.key());
    }

    // Ours and no longer driven: give it back to the surface.
    for (auto it = colourExclusions_.begin(); it != colourExclusions_.end();) {
        if (driven.find(*it) != driven.end()) {
            ++it;
            continue;
        }
        metasurface_.setExcluded(ParamAddress::fromKey(*it), false);
        it = colourExclusions_.erase(it);
    }

    // Driven and not yet ours: take it.
    for (const std::uint64_t key : driven) {
        if (colourExclusions_.find(key) != colourExclusions_.end()) continue;
        metasurface_.setExcluded(ParamAddress::fromKey(key), true);
        colourExclusions_.insert(key);
    }
}

gfx::Rect Application::layoutBrowser(gfx::Rect area) {
    if (!showBrowser_) return area;

    const Rect browserArea = area.removeFromLeft(browserWidthPx());
    browser_.render(ui_, browserArea);

    // Draggable splitter between the browser and whatever it sits beside.
    const Rect splitter{ area.left() - 2.0f, area.top(), 5.0f, area.height };
    bool hovered = false, held = false;
    ui_.buttonBehaviour(ui_.id("split.browser"), splitter, hovered, held);
    if (hovered || held) ui_.setCursor(ui::Cursor::ResizeHorizontal);
    if (ui_.isActive(ui_.id("split.browser"))) {
        browserWidth_ = clampValue(
            browserWidth_ + input_.mouseDelta.x / ui::theme().scale, 160.0f, 480.0f);
    }

    return area;
}

float Application::browserWidthPx() const { return ui::theme().scaled(browserWidth_); }
float Application::inspectorWidthPx() const { return ui::theme().scaled(inspectorWidth_); }
float Application::timelineHeightPx() const { return ui::theme().scaled(timelineHeight_); }

gfx::Rect Application::canvasBounds() const {
    Rect area{ 0.0f, 0.0f, static_cast<float>(window_.width()),
               static_cast<float>(window_.height()) };

    area.removeFromTop(ui::TransportBar::height());
    area.removeFromBottom(ui::StatusBar::height());
    if (showTimeline_) area.removeFromBottom(timelineHeightPx());
    if (showBrowser_) area.removeFromLeft(browserWidthPx());
    if (showInspector_) area.removeFromRight(inspectorWidthPx());

    return area;
}

void Application::layout(float deltaSeconds) {
    const ui::Theme& t = ui::theme();

    // Declared before anything is drawn. The settings sheet is drawn last so it
    // sits on top, and by then every view beneath it has already been offered
    // the frame's input - so the block has to be in place before they run.
    ui_.setModalActive(settings_.visible());

    Rect full{ 0.0f, 0.0f, static_cast<float>(window_.width()),
               static_cast<float>(window_.height()) };

    ui_.draw().addRectFilled(full, t.background);

    const Rect transportArea = full.removeFromTop(ui::TransportBar::height());
    const Rect statusArea = full.removeFromBottom(ui::StatusBar::height());

    transportBar_.render(ui_, transportArea, activeView_);

    switch (activeView_) {
        case ui::MainView::Patch: {
            Rect area = full;

            // Taken off the bottom before the side panels, so the timeline runs
            // the full width the way an arrangement view has to.
            if (showTimeline_) {
                const Rect timelineArea = area.removeFromBottom(timelineHeightPx());
                drawTimelinePlaceholder(timelineArea);
            }

            area = layoutBrowser(area);

            if (showInspector_) {
                const Rect inspectorArea = area.removeFromRight(inspectorWidthPx());
                inspector_.render(ui_, inspectorArea, patcher_.focusedNode());

                const Rect splitter{ inspectorArea.left() - 2.0f, inspectorArea.top(),
                                     5.0f, inspectorArea.height };
                bool hovered = false, held = false;
                ui_.buttonBehaviour(ui_.id("split.inspector"), splitter, hovered, held);
                if (hovered || held) ui_.setCursor(ui::Cursor::ResizeHorizontal);
                if (ui_.isActive(ui_.id("split.inspector")))
                    inspectorWidth_ = clampValue(
                        inspectorWidth_ - input_.mouseDelta.x / t.scale, 200.0f, 560.0f);
            }

            patcher_.render(ui_, area);
            patchView_.canvasX = patcher_.pan().x;
            patchView_.canvasY = patcher_.pan().y;
            patchView_.zoom = patcher_.zoom();
            break;
        }

        // The control tab is the played layout. The metasurface is still here,
        // but as one element a surface can place among its knobs rather than as
        // the whole tab.
        case ui::MainView::Control:
            controlView_.render(ui_, full);
            break;

        // Each of these takes the browser too. Songs and projects reference
        // audio by path and accept a dragged file directly; the stem browser is
        // where a folder is turned into a set, and having to leave it to go
        // and find one more file was the whole complaint.
        case ui::MainView::Stems:
            stemBrowser_.render(ui_, layoutBrowser(full));
            break;

        case ui::MainView::Projects:
            projectsView_.render(ui_, layoutBrowser(full));
            break;

        case ui::MainView::Songs:
            songsView_.render(ui_, layoutBrowser(full));
            break;

        case ui::MainView::Library:
            librarianView_.render(ui_, full);
            break;

        case ui::MainView::Count:
            break;

        case ui::MainView::Plugins:
            pluginView_.render(ui_, full);
            break;
    }

    statusBar_.render(ui_, statusArea, device_ ? device_->description() : std::string());

    // Drawn over everything, and it takes the frame's input when open so a
    // click meant for a combo inside it cannot also reach the canvas beneath.
    settings_.setControlPanelAvailable(device_ && device_->running()
                                       && device_->hasControlPanel());
    settings_.render(ui_, Rect{ 0.0f, 0.0f, static_cast<float>(window_.width()),
                                static_cast<float>(window_.height()) },
                     device_ ? device_->status() : platform::AudioDeviceStatus{});

    // The drag ghost follows the pointer above everything else.
    if (ui_.dragging()) {
        const std::string label = pathLeaf(ui_.dragPayload());
        const float width = ui_.font(t.fontSmall).textWidth(label) + 20.0f;
        const Rect ghost{ input_.mousePosition.x + 12.0f, input_.mousePosition.y + 12.0f,
                          width, 20.0f };
        ui_.overlay().addRectFilled(ghost, t.panelRaised.withAlpha(0.95f), t.cornerRadius);
        ui_.overlay().addRect(ghost, t.accent, 1.0f, t.cornerRadius);
        ui_.overlay().addTextClipped(ui_.font(t.fontSmall), ghost.deflated(6.0f), t.text, label);
    }

    (void)deltaSeconds;
}


void Application::drawLibraryPlaceholder(const gfx::Rect& bounds, ui::MainView view) {
    const ui::Theme& t = ui::theme();
    gfx::DrawList& list = ui_.draw();

    list.addRectFilled(bounds, t.background);
    gfx::Rect area = bounds.deflated(t.padding);

    ui_.label(area.removeFromTop(26.0f), ui::toString(view), t.text, t.fontTitle);
    area.removeFromTop(8.0f);

    // The store behind these is real and already on disk; the views are not
    // written yet. Saying which is which beats an empty rectangle.
    const int count = static_cast<int>(
        view == ui::MainView::Projects ? library_.entriesOfKind(library::EntryKind::Project).size()
      : view == ui::MainView::Songs    ? library_.entriesOfKind(library::EntryKind::Song).size()
                                       : library_.entries().size());

    char summary[256];
    std::snprintf(summary, sizeof(summary),
                  "%d entries in %s", count, library_.root().c_str());
    ui_.label(area.removeFromTop(18.0f), summary, t.textDim, t.fontUi);
    area.removeFromTop(4.0f);

    ui_.label(area.removeFromTop(18.0f),
              "the store is live and hand-editable; this view is still to come",
              t.textFaint, t.fontSmall);
}

void Application::drawTimelinePlaceholder(const gfx::Rect& bounds) {
    const ui::Theme& t = ui::theme();
    gfx::DrawList& list = ui_.draw();

    list.addRectFilled(bounds, t.panel);
    list.addRectFilled(gfx::Rect{ bounds.left(), bounds.top(), bounds.width, 1.0f }, t.border);

    gfx::Rect area = bounds.deflated(t.smallPadding);
    ui_.label(area.removeFromTop(16.0f), "timeline", t.textDim, t.fontUiBold);

    // A live bar ruler, so the strip is not merely an empty rectangle: it already
    // shows where the transport is, which is worth having on its own.
    gfx::Rect ruler = area.removeFromTop(22.0f);
    list.addRectFilled(ruler, t.canvas, t.cornerRadius);

    const TransportState state = engine_.transport().snapshot();
    const double beatsPerBar = static_cast<double>(std::max(1, state.timeSigNumerator));
    const double barsAcross = 32.0;
    const float barWidth = ruler.width / static_cast<float>(barsAcross);

    for (int bar = 0; bar <= static_cast<int>(barsAcross); ++bar) {
        const float x = ruler.left() + barWidth * static_cast<float>(bar);
        const bool major = (bar % 4) == 0;
        list.addRectFilled(gfx::Rect{ x, ruler.top(), 1.0f, major ? ruler.height : ruler.height * 0.4f },
                           major ? t.border : t.border.withAlpha(0.4f));
        if (major && barWidth > 12.0f) {
            list.addTextClipped(ui_.font(t.fontSmall),
                                gfx::Rect{ x + 3.0f, ruler.top(), barWidth, 12.0f },
                                t.textFaint, std::to_string(bar + 1));
        }
    }

    const double currentBar = std::fmod(state.ppqPosition / beatsPerBar, barsAcross);
    const float playheadX = ruler.left() + barWidth * static_cast<float>(currentBar);
    list.addRectFilled(gfx::Rect{ playheadX - 1.0f, ruler.top(), 2.0f, ruler.height }, t.accent);

    list.addTextClipped(ui_.font(t.fontSmall), area, t.textFaint,
                        "arrangement lanes go here - Ctrl+3 to hide",
                        gfx::DrawList::Align::Centre);
}

void Application::handleGlobalShortcuts() {
    if (ui_.keyboardCaptured()) return;
    // While the settings sheet is up it owns the keyboard, apart from Escape.
    if (settings_.visible()) {
        if (input_.keyPressed(ui::key::Escape)) settings_.close();
        return;
    }

    const ui::InputState& in = input_;

    if (in.ctrl && in.keyPressed(ui::key::N)) newPatch();
    if (in.ctrl && in.keyPressed(ui::key::O)) openPatch();
    if (in.ctrl && in.keyPressed(ui::key::S)) {
        if (in.shift) savePatchAs();
        else savePatch();
    }

    // Space is the transport, everywhere except inside a text field.
    if (in.keyPressed(ui::key::Space) && !in.ctrl)
        engine_.transport().togglePlaying();

    // The four tabs a set is played from lead, in the order they are reached
    // for; the three library tabs follow. Not tab order: F1 has meant the patch
    // since before the library tabs existed, and renumbering the keys somebody
    // has in their fingers to tidy a list is not worth it.
    if (in.keyPressed(ui::key::F1 + 0)) activeView_ = ui::MainView::Patch;
    if (in.keyPressed(ui::key::F1 + 1)) activeView_ = ui::MainView::Control;
    if (in.keyPressed(ui::key::F1 + 2)) activeView_ = ui::MainView::Plugins;
    if (in.keyPressed(ui::key::F1 + 3)) activeView_ = ui::MainView::Stems;
    if (in.keyPressed(ui::key::F1 + 4)) activeView_ = ui::MainView::Projects;
    if (in.keyPressed(ui::key::F1 + 5)) activeView_ = ui::MainView::Songs;
    if (in.keyPressed(ui::key::F1 + 6)) activeView_ = ui::MainView::Library;

    // Ctrl+comma is the conventional settings shortcut on every platform.
    if (in.ctrl && in.keyPressed(0xBC)) settings_.open();

    if (in.ctrl && in.keyPressed('1')) showBrowser_ = !showBrowser_;
    if (in.ctrl && in.keyPressed('2')) showInspector_ = !showInspector_;
    if (in.ctrl && in.keyPressed('3')) showTimeline_ = !showTimeline_;

    // The metasurface is worth reaching without changing view: capture a
    // snapshot from anywhere.
    if (in.ctrl && in.keyPressed(ui::key::R)) {
        metasurfaceView_.captureHere(metasurface_.cursor());
        ui_.notify("snapshot captured", ui::theme().accent, 2.0f);
        markModified();
    }
}

// ---------------------------------------------------------------------------
// Patch management
// ---------------------------------------------------------------------------

void Application::updateWindowTitle() {
    const std::string name = patchPath_.empty() ? "untitled" : pathStem(patchPath_);
    window_.setTitle("acomposter - " + name + (modified_ ? " *" : ""));
    transportBar_.setPatchName(name);
    transportBar_.setModified(modified_);
}

void Application::newPatch() {
    if (!confirmDiscardChanges()) return;

    closeAllPluginEditors();
    patch::buildDefaultPatch(engine_, metasurface_);

    // The surface goes with the patch. Keeping the last one would leave every
    // control bound to node ids that now belong to different nodes, which is
    // worse than an empty surface by some distance.
    surface_.clear();
    controlView_.setEditing(false);

    patchPath_.clear();
    patchMetadata_ = PatchMetadata{};
    modified_ = false;

    // The claims went with the patch. Keeping them would leave the set holding
    // node ids that now belong to different nodes, and the next reconciliation
    // would release parameters on this patch that it never took.
    colourExclusions_.clear();

    patcher_.clearSelection();
    patcher_.resetView();
    updateWindowTitle();
}

void Application::openPatch() {
    if (!confirmDiscardChanges()) return;

    const std::string path = platform::openFileDialog(window_.handle(), "Open patch",
                                                       patchFilters(), paths::patchesDirectory());
    if (!path.empty()) openPatchFile(path);
}

void Application::openPatchFile(const std::string& path) {
    if (!confirmDiscardChanges()) return;

    closeAllPluginEditors();

    const PatchLoadResult result =
        patch::loadFromFile(path, engine_, metasurface_, surface_, patchView_, patchMetadata_);

    if (!result.ok) {
        platform::messageDialog(window_.handle(), "Could not open patch", result.error, true);
        return;
    }

    patchPath_ = path;
    modified_ = false;

    patcher_.clearSelection();
    // Every control shows where its parameter is in the patch just loaded.
    surface_.adoptAllFromGraph(engine_.graph());
    // The colour targets came back with the patch, so the exclusions they own
    // have to be re-derived: the file records which parameters are excluded but
    // not which of them the colour knob claimed.
    colourExclusions_.clear();
    reconcileColourExclusions();

    patcher_.setPan({ patchView_.canvasX, patchView_.canvasY });
    patcher_.setZoom(patchView_.zoom);
    updateWindowTitle();

    if (!result.warnings.empty()) {
        // Warnings are worth showing but must not block: a patch that lost one
        // plugin should still open and play.
        std::string summary = result.warnings.front();
        if (result.warnings.size() > 1)
            summary += "  (+" + std::to_string(result.warnings.size() - 1) + " more)";
        ui_.notify(summary, ui::theme().warning, 8.0f);
    } else {
        ui_.notify("opened " + pathStem(path), ui::theme().accent, 2.5f);
    }
}

bool Application::savePatch() {
    if (patchPath_.empty()) return savePatchAs();

    std::string error;
    if (!patch::saveToFile(patchPath_, engine_, metasurface_, surface_, patchView_,
                           patchMetadata_, &error)) {
        platform::messageDialog(window_.handle(), "Could not save patch", error, true);
        return false;
    }

    modified_ = false;
    updateWindowTitle();
    ui_.notify("saved " + pathStem(patchPath_), ui::theme().accent, 2.0f);
    return true;
}

bool Application::savePatchAs() {
    const std::string suggested = patchPath_.empty() ? "untitled" : pathStem(patchPath_);

    const std::string path = platform::saveFileDialog(
        window_.handle(), "Save patch", patchFilters(), paths::patchesDirectory(),
        suggested, patch::kFileExtension);

    if (path.empty()) return false;

    patchPath_ = path;
    return savePatch();
}

bool Application::confirmDiscardChanges() {
    if (!modified_) return true;

    const std::string name = patchPath_.empty() ? "untitled" : pathStem(patchPath_);
    switch (platform::askToSaveChanges(window_.handle(), name)) {
        case platform::SaveChangesResult::Save:    return savePatch();
        case platform::SaveChangesResult::Discard: return true;
        case platform::SaveChangesResult::Cancel:  return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Plugins
// ---------------------------------------------------------------------------

void Application::beginStemEffectPick(NodeId stemPlayer, int slot) {
    pendingChainPlayer_ = stemPlayer;
    pendingChainSlot_ = slot;

    activeView_ = ui::MainView::Plugins;
    ui_.notify("pick a plugin - it will be added to that stem's rack",
               ui::theme().accent, 5.0f);
}

void Application::addPluginNode(const vst2::PluginDescription& description, bool forceBridge) {
    // A pick that was started from a stem's rack goes there instead of onto the
    // canvas, wired in at the end of whatever is already on that stem.
    if (pendingChainPlayer_ != kInvalidNode && pendingChainSlot_ >= 0) {
        const NodeId player = pendingChainPlayer_;
        const int slot = pendingChainSlot_;
        pendingChainPlayer_ = kInvalidNode;
        pendingChainSlot_ = -1;

        const NodeId added = patcher_.addToStemChain(player, slot, description, forceBridge);
        activeView_ = ui::MainView::Patch;

        if (added != kInvalidNode) {
            markModified();
            const Node* node = engine_.graph().node(added);
            const bool failed = node && !node->errorText().empty();
            ui_.notify(failed ? description.name + ": " + node->errorText()
                              : "added " + description.name + " to the rack",
                       failed ? ui::theme().danger : ui::theme().accent, failed ? 8.0f : 2.5f);
        } else {
            ui_.notify("could not add " + description.name + " to that rack",
                       ui::theme().danger, 6.0f);
        }
        return;
    }

    auto node = std::make_unique<vst2::VstNode>(description);

    std::string error;
    const bool loaded = node->loadPlugin(plugins_, engine_.stats().sampleRate,
                                         std::max(64, engine_.stats().blockSize),
                                         forceBridge, &error);

    // The node is placed even when the plugin would not load: the error is
    // visible on the box, which is far more useful than a dialog and nothing.
    // placeNode drops it in the middle of the canvas and selects it.
    patcher_.placeNode(std::move(node), patcher_.defaultDropPosition(canvasBounds()));
    markModified();

    // Switch to the canvas either way. Staying on the plugin list after a failed
    // load leaves the new node somewhere the user cannot see, which reads as the
    // button having done nothing at all.
    activeView_ = ui::MainView::Patch;

    if (loaded) ui_.notify("added " + description.name, ui::theme().accent, 2.5f);
    else ui_.notify(description.name + ": " + error, ui::theme().danger, 8.0f);
}

void Application::togglePluginEditor(NodeId nodeId) {
    if (auto* plugin = dynamic_cast<vst2::VstNode*>(engine_.graph().node(nodeId)))
        plugin->toggleEditor();
}

void Application::closeAllPluginEditors() {
    for (const auto& node : engine_.graph().nodes())
        if (auto* plugin = dynamic_cast<vst2::VstNode*>(node.get()))
            plugin->closeEditor();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void Application::loadSettings() {
    std::string text;
    if (!readFileText(paths::settingsFile(), text)) return;

    std::string parseError;
    const JsonValue root = JsonValue::parse(text, &parseError);
    if (!parseError.empty() || !root.isObject()) return;

    if (const JsonValue* audio = root.find("audio")) {
        deviceSettings_.backend = platform::audioBackendFromString(audio->getString("backend"));
        deviceSettings_.outputDeviceId = audio->getString("outputDeviceId");
        deviceSettings_.inputDeviceId = audio->getString("inputDeviceId");
        deviceSettings_.enableInput = audio->getBool("enableInput", true);
        deviceSettings_.blockSize = clampValue(audio->getInt("blockSize", 256), 32, 4096);
        deviceSettings_.outputChannelCount = clampValue(audio->getInt("outputChannelCount", 0), 0, 256);
        deviceSettings_.outputChannelOffset = clampValue(audio->getInt("outputChannelOffset", 0), 0, 255);
    }

    if (const JsonValue* interfaceSettings = root.find("interface")) {
        browserWidth_ = clampValue(interfaceSettings->getFloat("browserWidth", 230.0f), 160.0f, 480.0f);
        inspectorWidth_ = clampValue(interfaceSettings->getFloat("inspectorWidth", 280.0f), 200.0f, 560.0f);
        showBrowser_ = interfaceSettings->getBool("showBrowser", true);
        showInspector_ = interfaceSettings->getBool("showInspector", true);
        showTimeline_ = interfaceSettings->getBool("showTimeline", false);
        timelineHeight_ = clampValue(interfaceSettings->getFloat("timelineHeight", 150.0f),
                                     80.0f, 460.0f);
        vsync_ = interfaceSettings->getBool("vsync", true);

        // Where the sample library is does not change between sessions, and
        // walking back to it from Documents every launch gets old fast.
        if (const std::string directory = interfaceSettings->getString("browserDirectory");
            !directory.empty())
            browser_.navigateTo(directory);
    }

    if (const JsonValue* master = root.find("master")) {
        engine_.setMasterGainDb(master->getFloat("gainDb", 0.0f));
        engine_.setMasterLimiterEnabled(master->getBool("limiter", true));
    }
}

void Application::saveSettings() const {
    JsonValue root = JsonValue::object();
    root.set("format", "acomposter-settings");
    root.set("version", 1);

    JsonValue audio = JsonValue::object();
    audio.set("backend", platform::toString(deviceSettings_.backend));
    audio.set("outputDeviceId", deviceSettings_.outputDeviceId);
    audio.set("inputDeviceId", deviceSettings_.inputDeviceId);
    audio.set("enableInput", deviceSettings_.enableInput);
    audio.set("blockSize", deviceSettings_.blockSize);
    audio.set("outputChannelCount", deviceSettings_.outputChannelCount);
    audio.set("outputChannelOffset", deviceSettings_.outputChannelOffset);
    root.set("audio", audio);

    JsonValue interfaceSettings = JsonValue::object();
    interfaceSettings.set("browserWidth", browserWidth_);
    interfaceSettings.set("inspectorWidth", inspectorWidth_);
    interfaceSettings.set("showBrowser", showBrowser_);
    interfaceSettings.set("showInspector", showInspector_);
    interfaceSettings.set("showTimeline", showTimeline_);
    interfaceSettings.set("timelineHeight", timelineHeight_);
    interfaceSettings.set("vsync", vsync_);
    interfaceSettings.set("browserDirectory", browser_.currentDirectory());
    root.set("interface", interfaceSettings);

    JsonValue master = JsonValue::object();
    master.set("gainDb", engine_.masterGainDb());
    master.set("limiter", engine_.masterLimiterEnabled());
    root.set("master", master);

    writeFileText(paths::settingsFile(), root.dump(2));
}

} // namespace acm::app
