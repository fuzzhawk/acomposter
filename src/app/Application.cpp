#include "Application.h"

#include "../core/AppPaths.h"
#include "../core/FileIo.h"
#include "../core/Json.h"
#include "../nodes/NodeFactory.h"
#include "../nodes/SamplePlayerNode.h"
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
    browser_.initialise();
    inspector_.initialise(&engine_, &metasurface_);
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
    inspector_.onTidyChains = [this](NodeId player) {
        patcher_.tidyStemChains(player);
        markModified();
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
}

gfx::Rect Application::canvasBounds() const {
    Rect area{ 0.0f, 0.0f, static_cast<float>(window_.width()),
               static_cast<float>(window_.height()) };

    area.removeFromTop(ui::TransportBar::height());
    area.removeFromBottom(ui::StatusBar::height());
    if (showTimeline_) area.removeFromBottom(timelineHeight_);
    if (showBrowser_) area.removeFromLeft(browserWidth_);
    if (showInspector_) area.removeFromRight(inspectorWidth_);

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
                const Rect timelineArea = area.removeFromBottom(timelineHeight_);
                drawTimelinePlaceholder(timelineArea);
            }

            if (showBrowser_) {
                const Rect browserArea = area.removeFromLeft(browserWidth_);
                browser_.render(ui_, browserArea);

                // Draggable splitter between the browser and the canvas.
                const Rect splitter{ area.left() - 2.0f, area.top(), 5.0f, area.height };
                bool hovered = false, held = false;
                ui_.buttonBehaviour(ui_.id("split.browser"), splitter, hovered, held);
                if (hovered || held) ui_.setCursor(ui::Cursor::ResizeHorizontal);
                if (ui_.isActive(ui_.id("split.browser")))
                    browserWidth_ = clampValue(browserWidth_ + input_.mouseDelta.x, 160.0f, 480.0f);
            }

            if (showInspector_) {
                const Rect inspectorArea = area.removeFromRight(inspectorWidth_);
                inspector_.render(ui_, inspectorArea, patcher_.focusedNode());

                const Rect splitter{ inspectorArea.left() - 2.0f, inspectorArea.top(),
                                     5.0f, inspectorArea.height };
                bool hovered = false, held = false;
                ui_.buttonBehaviour(ui_.id("split.inspector"), splitter, hovered, held);
                if (hovered || held) ui_.setCursor(ui::Cursor::ResizeHorizontal);
                if (ui_.isActive(ui_.id("split.inspector")))
                    inspectorWidth_ = clampValue(inspectorWidth_ - input_.mouseDelta.x, 200.0f, 560.0f);
            }

            patcher_.render(ui_, area);
            patchView_.canvasX = patcher_.pan().x;
            patchView_.canvasY = patcher_.pan().y;
            patchView_.zoom = patcher_.zoom();
            break;
        }

        case ui::MainView::Metasurface:
            metasurfaceView_.render(ui_, full);
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

    if (in.keyPressed(ui::key::F1)) activeView_ = ui::MainView::Patch;
    if (in.keyPressed(ui::key::F1 + 1)) activeView_ = ui::MainView::Metasurface;
    if (in.keyPressed(ui::key::F1 + 2)) activeView_ = ui::MainView::Plugins;

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

    patchPath_.clear();
    patchMetadata_ = PatchMetadata{};
    modified_ = false;

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
        patch::loadFromFile(path, engine_, metasurface_, patchView_, patchMetadata_);

    if (!result.ok) {
        platform::messageDialog(window_.handle(), "Could not open patch", result.error, true);
        return;
    }

    patchPath_ = path;
    modified_ = false;

    patcher_.clearSelection();
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
    if (!patch::saveToFile(patchPath_, engine_, metasurface_, patchView_, patchMetadata_, &error)) {
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
