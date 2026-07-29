#include "Panels.h"

#include "../audio/AudioFile.h"
#include "../core/AppPaths.h"
#include "../core/FileIo.h"
#include "../patch/Patch.h"
#include "../vst2/VstNode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace acm::ui {
namespace {

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;

    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + k]))
                != std::tolower(static_cast<unsigned char>(needle[k]))) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

std::string humanSize(std::int64_t bytes) {
    char buffer[32];
    if (bytes < 1024) std::snprintf(buffer, sizeof(buffer), "%lld B", static_cast<long long>(bytes));
    else if (bytes < 1024 * 1024)
        std::snprintf(buffer, sizeof(buffer), "%.0f kB", static_cast<double>(bytes) / 1024.0);
    else
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

} // namespace

// ---------------------------------------------------------------------------
// BrowserView
// ---------------------------------------------------------------------------

void BrowserView::initialise() {
    places_.push_back({ "patches", paths::patchesDirectory() });
    places_.push_back({ "recordings", paths::recordingsDirectory() });
    places_.push_back({ "documents", paths::documents() });

    currentDirectory_ = paths::patchesDirectory();
    needsRefresh_ = true;
}

void BrowserView::navigateTo(const std::string& directory) {
    if (directory.empty()) return;
    currentDirectory_ = directory;
    needsRefresh_ = true;
}

void BrowserView::refresh() {
    entries_.clear();

    // Audio and patch files only: this is a browser for the two things that can
    // be dropped on the canvas, not a file manager.
    std::vector<std::string> extensions = audiofile::supportedExtensions();
    extensions.push_back(patch::kFileExtension);

    entries_ = listDirectory(currentDirectory_, extensions);
    needsRefresh_ = false;
}

void BrowserView::drawPlaces(Ui& ui, Rect& area) {
    Rect row = area.removeFromTop(20.0f);
    for (const Place& place : places_) {
        const float width = std::min(76.0f, row.width / static_cast<float>(places_.size()));
        const Rect button = row.removeFromLeft(width);

        if (ui.button(ui.id("browser.place." + place.name), button, place.name,
                      Ui::ButtonStyle::Ghost))
            navigateTo(place.path);
    }
    area.removeFromTop(4.0f);
}

void BrowserView::render(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();
    if (needsRefresh_) refresh();

    ui.draw().addRectFilled(bounds, t.panel);
    ui.draw().addRectFilled(Rect{ bounds.right() - 1.0f, bounds.top(), 1.0f, bounds.height },
                            t.border);

    Rect area = bounds.deflated(t.smallPadding);

    ui.label(area.removeFromTop(18.0f), "browser", t.textDim, t.fontUiBold);
    area.removeFromTop(2.0f);

    drawPlaces(ui, area);

    // -- path row ----------------------------------------------------------
    Rect pathRow = area.removeFromTop(22.0f);
    const Rect upButton = pathRow.removeFromLeft(24.0f);
    pathRow.removeFromLeft(4.0f);

    if (ui.iconButton(ui.id("browser.up"), upButton, Ui::Icon::Chevron, t.textDim)) {
        const std::string parent = pathParent(currentDirectory_);
        if (!parent.empty() && parent != currentDirectory_) navigateTo(parent);
    }
    if (ui.isHot(ui.id("browser.up"))) ui.setTooltip("Go up one folder");

    ui.draw().addTextClipped(ui.font(t.fontSmall), pathRow, t.textFaint,
                             pathLeaf(currentDirectory_).empty() ? currentDirectory_
                                                                 : pathLeaf(currentDirectory_));
    area.removeFromTop(4.0f);

    // -- filter ------------------------------------------------------------
    Rect filterRow = area.removeFromTop(22.0f);
    ui.textField(ui.id("browser.filter"), filterRow, filter_, "filter");
    area.removeFromTop(6.0f);

    // -- listing -----------------------------------------------------------
    std::vector<const DirectoryEntry*> visible;
    visible.reserve(entries_.size());
    for (const DirectoryEntry& entry : entries_)
        if (containsIgnoreCase(entry.name, filter_)) visible.push_back(&entry);

    const float rowHeight = 21.0f;
    Rect content = ui.beginScroll(ui.id("browser.list"), area,
                                  static_cast<float>(visible.size()) * rowHeight);

    for (const DirectoryEntry* entry : visible) {
        const Rect row = content.removeFromTop(rowHeight);
        if (row.bottom() < area.top() - rowHeight || row.top() > area.bottom() + rowHeight)
            continue;   // scrolled out of view

        const UiId rowId = ui.idFrom(entry, 1);
        bool hovered = false, held = false;
        const bool clicked = ui.buttonBehaviour(rowId, row, hovered, held);

        if (hovered) ui.draw().addRectFilled(row, t.widgetHover, t.cornerRadius);

        Rect rowContent = row.deflated(3.0f);
        const Rect iconArea = rowContent.removeFromLeft(16.0f);
        rowContent.removeFromLeft(4.0f);

        const bool isPatch = !entry->isDirectory
                          && pathExtension(entry->name) == patch::kFileExtension;

        ui.drawIcon(ui.draw(), iconArea,
                    entry->isDirectory ? Ui::Icon::Folder
                    : isPatch ? Ui::Icon::Grid : Ui::Icon::Wave,
                    entry->isDirectory ? t.control : (isPatch ? t.accent : t.textDim));

        if (!entry->isDirectory) {
            const Rect sizeArea = rowContent.removeFromRight(52.0f);
            ui.draw().addTextClipped(ui.font(t.fontSmall), sizeArea, t.textFaint,
                                     humanSize(entry->size), DrawList::Align::Right);
        }

        ui.draw().addTextClipped(ui.font(t.fontUi), rowContent,
                                 hovered ? t.text : t.textDim, entry->name);

        // Press-and-move on an audio file starts a drag onto the canvas.
        if (held && !entry->isDirectory && !isPatch && !ui.dragging()
            && (ui.input().mousePosition - row.centre()).lengthSquared() > 16.0f) {
            ui.beginDrag("file", entry->fullPath);
        }

        if (clicked) {
            if (entry->isDirectory) {
                navigateTo(entry->fullPath);
            } else if (isPatch) {
                if (onOpenPatch) onOpenPatch(entry->fullPath);
            } else if (ui.input().mouseDoubleClicked[static_cast<int>(MouseButton::Left)]) {
                if (onLoadSample) onLoadSample(entry->fullPath);
            }
        }
    }

    ui.endScroll();
}

// ---------------------------------------------------------------------------
// InspectorView
// ---------------------------------------------------------------------------

void InspectorView::initialise(Engine* engine, Metasurface* metasurface) {
    engine_ = engine;
    metasurface_ = metasurface;
}

void InspectorView::drawPluginSection(Ui& ui, Rect& area, Node& node) {
    const Theme& t = theme();
    auto* plugin = dynamic_cast<vst2::VstNode*>(&node);
    if (!plugin) return;

    const auto& description = plugin->pluginDescription();

    ui.separator(area.removeFromTop(9.0f));
    ui.label(area.removeFromTop(16.0f), "plugin", t.textDim, t.fontUiBold);

    char detail[192];
    std::snprintf(detail, sizeof(detail), "%s  %s  %d in / %d out%s",
                  description.vendor.empty() ? "unknown vendor" : description.vendor.c_str(),
                  vst2::toString(description.architecture),
                  description.numInputs, description.numOutputs,
                  plugin->bridged() ? "  bridged" : "");
    ui.label(area.removeFromTop(15.0f), detail, t.textFaint, t.fontSmall);

    Rect buttons = area.removeFromTop(22.0f);
    if (ui.button(ui.id("inspector.editor"), buttons.removeFromLeft(buttons.width * 0.5f - 3.0f),
                  plugin->editorOpen() ? "close editor" : "open editor",
                  Ui::ButtonStyle::Toggle, plugin->editorOpen(),
                  plugin->pluginLoaded() && description.hasEditor)) {
        if (onOpenPluginEditor) onOpenPluginEditor(node.id());
    }
    buttons.removeFromLeft(6.0f);

    if (ui.button(ui.id("inspector.reload"), buttons, "reload", Ui::ButtonStyle::Normal,
                  false, plugin->pluginLoaded() || !description.path.empty()))
        plugin->reloadPlugin();

    area.removeFromTop(6.0f);

    // Programs, if the plugin has any worth showing.
    const int programCount = plugin->programCount();
    if (programCount > 1) {
        Rect programRow = area.removeFromTop(22.0f);
        ui.label(programRow.removeFromLeft(56.0f), "preset", t.textDim, t.fontSmall);

        // Names are fetched lazily: a bridged plugin with 128 programs would
        // otherwise cost 128 IPC round trips per frame.
        static std::vector<std::string> programNames;
        static NodeId cachedFor = kInvalidNode;
        static int cachedCount = 0;
        if (cachedFor != node.id() || cachedCount != programCount) {
            programNames.clear();
            for (int i = 0; i < std::min(programCount, 256); ++i)
                programNames.push_back(plugin->programName(i));
            cachedFor = node.id();
            cachedCount = programCount;
        }

        int current = plugin->currentProgram();
        if (ui.combo(ui.id("inspector.program"), programRow, programNames, current))
            plugin->setCurrentProgram(current);

        area.removeFromTop(6.0f);
    }
}

void InspectorView::drawParameterList(Ui& ui, Rect area, Node& node) {
    const Theme& t = theme();
    const Colour accent = t.categoryColour(node.category());

    // Count what will be drawn so the scroller knows its content height.
    int rows = 0;
    for (int i = 0; i < node.numParameters(); ++i) rows += node.parameter(i).automatable() ? 1 : 0;

    const float rowHeight = 23.0f;
    Rect content = ui.beginScroll(ui.id("inspector.params"), area,
                                  static_cast<float>(rows) * rowHeight + 4.0f);

    for (int i = 0; i < node.numParameters(); ++i) {
        Parameter& parameter = node.parameter(i);
        if (!parameter.automatable()) continue;

        Rect row = content.removeFromTop(rowHeight);
        if (row.bottom() < area.top() - rowHeight || row.top() > area.bottom() + rowHeight)
            continue;

        // A small square on the left toggles whether the metasurface owns this
        // parameter: freezing one control while the rest of the patch morphs is
        // one of the most useful things the surface can do.
        const Rect lockArea = row.removeFromLeft(16.0f);
        row.removeFromLeft(4.0f);

        if (metasurface_) {
            const ParamAddress address{ node.id(), i };
            const bool excluded = metasurface_->isExcluded(address);

            if (ui.iconButton(ui.idFrom(&parameter, 7), lockArea,
                              excluded ? Ui::Icon::Cross : Ui::Icon::Target,
                              excluded ? t.textFaint : t.accent, !excluded))
                metasurface_->setExcluded(address, !excluded);

            if (ui.isHot(ui.idFrom(&parameter, 7)))
                ui.setTooltip(excluded ? "Excluded from the metasurface"
                                       : "Interpolated by the metasurface");
        }

        ui.parameterRow(row, parameter, accent);
    }

    ui.endScroll();
}

void InspectorView::render(Ui& ui, const Rect& bounds, NodeId nodeId) {
    const Theme& t = theme();

    ui.draw().addRectFilled(bounds, t.panel);
    ui.draw().addRectFilled(Rect{ bounds.left(), bounds.top(), 1.0f, bounds.height }, t.border);

    Rect area = bounds.deflated(t.smallPadding);
    ui.label(area.removeFromTop(18.0f), "inspector", t.textDim, t.fontUiBold);
    area.removeFromTop(2.0f);

    Node* node = engine_ ? engine_->graph().node(nodeId) : nullptr;
    if (!node) {
        ui.draw().addTextClipped(ui.font(t.fontSmall), area.removeFromTop(40.0f), t.textFaint,
                                 "select a node", DrawList::Align::Centre);
        return;
    }

    // -- identity ----------------------------------------------------------
    Rect nameRow = area.removeFromTop(24.0f);
    if (nameBufferFor_ != nodeId) { nameBuffer_ = node->name(); nameBufferFor_ = nodeId; }
    if (ui.textField(ui.id("inspector.name"), nameRow, nameBuffer_, "node name") && !nameBuffer_.empty())
        node->setName(nameBuffer_);

    Rect typeRow = area.removeFromTop(15.0f);
    char typeText[160];
    std::snprintf(typeText, sizeof(typeText), "%s  -  %s",
                  node->typeName().c_str(), toString(node->category()));
    ui.label(typeRow, typeText, t.textFaint, t.fontSmall);
    area.removeFromTop(4.0f);

    // -- state -------------------------------------------------------------
    Rect stateRow = area.removeFromTop(22.0f);
    bool bypassed = node->bypassed();
    if (ui.checkbox(ui.id("inspector.bypass"), stateRow.removeFromLeft(90.0f), "bypass", bypassed))
        node->setBypassed(bypassed);

    if (node->latencyFrames() > 0) {
        char latency[64];
        std::snprintf(latency, sizeof(latency), "%d frames latency", node->latencyFrames());
        ui.label(stateRow, latency, t.warning, t.fontSmall, DrawList::Align::Right);
    }

    if (!node->errorText().empty()) {
        const Rect errorRow = area.removeFromTop(30.0f);
        ui.draw().addRectFilled(errorRow, t.danger.withAlpha(0.10f), t.cornerRadius);
        ui.draw().addTextClipped(ui.font(t.fontSmall), errorRow.deflated(4.0f), t.danger,
                                 node->errorText());
        area.removeFromTop(4.0f);
    }

    drawPluginSection(ui, area, *node);

    // -- comment -----------------------------------------------------------
    ui.separator(area.removeFromTop(9.0f));
    Rect commentRow = area.removeFromTop(22.0f);
    if (commentBufferFor_ != nodeId) { commentBuffer_ = node->comment; commentBufferFor_ = nodeId; }
    if (ui.textField(ui.id("inspector.comment"), commentRow, commentBuffer_, "note"))
        node->comment = commentBuffer_;

    area.removeFromTop(6.0f);
    ui.separator(area.removeFromTop(9.0f));
    ui.label(area.removeFromTop(16.0f), "parameters", t.textDim, t.fontUiBold);
    area.removeFromTop(2.0f);

    drawParameterList(ui, area, *node);
}

// ---------------------------------------------------------------------------
// PluginManagerView
// ---------------------------------------------------------------------------

void PluginManagerView::initialise(vst2::PluginManager* manager) {
    manager_ = manager;
}

void PluginManagerView::render(Ui& ui, const Rect& bounds) {
    if (!manager_) return;

    const Theme& t = theme();
    ui.draw().addRectFilled(bounds, t.background);

    Rect area = bounds.deflated(t.padding);

    // -- header ------------------------------------------------------------
    Rect header = area.removeFromTop(26.0f);
    ui.label(header.removeFromLeft(160.0f), "vst2 plugins", t.text, t.fontTitle);

    const bool scanning = manager_->scanning();

    if (ui.button(ui.id("plugins.scan"), header.removeFromRight(96.0f),
                  scanning ? "cancel" : "scan", scanning ? Ui::ButtonStyle::Danger
                                                          : Ui::ButtonStyle::Primary)) {
        if (scanning) manager_->cancelScan();
        else manager_->startScan(false);
    }
    header.removeFromRight(6.0f);

    if (ui.button(ui.id("plugins.rescan"), header.removeFromRight(96.0f), "rescan all",
                  Ui::ButtonStyle::Normal, false, !scanning))
        manager_->startScan(true);
    header.removeFromRight(6.0f);

    if (ui.button(ui.id("plugins.addpath"), header.removeFromRight(110.0f), "add folder",
                  Ui::ButtonStyle::Normal, false, !scanning && onBrowseForFolder != nullptr)) {
        const std::string folder = onBrowseForFolder ? onBrowseForFolder() : std::string();
        if (!folder.empty()) manager_->addSearchPath(folder);
    }

    area.removeFromTop(6.0f);

    // -- scan progress -----------------------------------------------------
    if (scanning) {
        const vst2::ScanProgress progress = manager_->progress();
        Rect progressRow = area.removeFromTop(20.0f);

        const float fraction = progress.filesFound > 0
            ? static_cast<float>(progress.filesScanned) / static_cast<float>(progress.filesFound)
            : 0.0f;

        ui.draw().addRectFilled(progressRow, t.widgetTrack, t.cornerRadius);
        ui.draw().addRectFilled(Rect{ progressRow.left(), progressRow.top(),
                                      progressRow.width * fraction, progressRow.height },
                                t.accentDim, t.cornerRadius);

        char text[256];
        std::snprintf(text, sizeof(text), "%d / %d   %d found   %d failed   %s",
                      progress.filesScanned, progress.filesFound,
                      progress.pluginsFound, progress.failures, progress.currentFile.c_str());
        ui.draw().addTextClipped(ui.font(t.fontSmall), progressRow.deflated(4.0f), t.text, text);
        area.removeFromTop(6.0f);
    }

    // -- search paths ------------------------------------------------------
    Rect pathsRow = area.removeFromTop(18.0f);
    ui.label(pathsRow, "search folders", t.textDim, t.fontSmall);

    for (const std::string& path : manager_->searchPaths()) {
        if (area.height < 80.0f) break;
        Rect row = area.removeFromTop(17.0f);

        const Rect removeArea = row.removeFromRight(18.0f);
        if (ui.iconButton(ui.idFrom(&path, 3), removeArea, Ui::Icon::Cross, t.textFaint))
            manager_->removeSearchPath(path);

        ui.draw().addTextClipped(ui.font(t.fontSmall), row, t.textFaint, path);
    }

    area.removeFromTop(6.0f);
    ui.separator(area.removeFromTop(9.0f));

    // -- filter and options ------------------------------------------------
    Rect toolRow = area.removeFromTop(24.0f);
    ui.textField(ui.id("plugins.search"), toolRow.removeFromLeft(260.0f), search_, "search plugins");
    toolRow.removeFromLeft(12.0f);

    ui.checkbox(ui.id("plugins.bridge"), toolRow.removeFromLeft(190.0f),
                "always bridge (isolate)", forceBridge_);
    if (ui.isHot(ui.id("plugins.bridge")))
        ui.setTooltip("Run even same-architecture plugins in a helper process, so a crash "
                      "cannot take acomposter with it");

    ui.checkbox(ui.id("plugins.failures"), toolRow.removeFromLeft(150.0f),
                "show failures", showFailures_);

    area.removeFromTop(6.0f);

    // -- listing -----------------------------------------------------------
    const std::vector<vst2::PluginDescription> plugins = manager_->plugins();
    const std::vector<vst2::FailedPlugin> failures = manager_->failures();

    std::vector<const vst2::PluginDescription*> visible;
    for (const auto& plugin : plugins) {
        if (containsIgnoreCase(plugin.name, search_) || containsIgnoreCase(plugin.vendor, search_))
            visible.push_back(&plugin);
    }

    const float rowHeight = 26.0f;
    const float contentHeight = static_cast<float>(visible.size()) * rowHeight
                              + (showFailures_ ? static_cast<float>(failures.size()) * 20.0f + 26.0f : 0.0f);

    Rect content = ui.beginScroll(ui.id("plugins.list"), area, contentHeight);

    if (visible.empty() && !scanning) {
        ui.draw().addTextClipped(ui.font(t.fontUi), content.removeFromTop(48.0f), t.textFaint,
                                 plugins.empty() ? "no plugins found yet - press scan"
                                                 : "nothing matches that search",
                                 DrawList::Align::Centre);
    }

    for (std::size_t i = 0; i < visible.size(); ++i) {
        const vst2::PluginDescription& plugin = *visible[i];
        const Rect row = content.removeFromTop(rowHeight);
        if (row.bottom() < area.top() - rowHeight || row.top() > area.bottom() + rowHeight) continue;

        const UiId rowId = ui.idFrom(&plugin, 1);
        bool hovered = false, held = false;
        const bool clicked = ui.buttonBehaviour(rowId, row, hovered, held);

        const bool isSelected = static_cast<int>(i) == selectedIndex_;
        if (isSelected) ui.draw().addRectFilled(row, t.accent.withAlpha(0.10f), t.cornerRadius);
        else if (hovered) ui.draw().addRectFilled(row, t.widgetHover, t.cornerRadius);

        Rect rowContent = row.deflated(4.0f);

        // Architecture badge: the single most useful thing to see at a glance,
        // because it determines whether the plugin will be bridged.
        const Rect badge = rowContent.removeFromRight(46.0f);
        const bool needsBridge = vst2::PluginManager::requiresBridge(plugin.architecture);
        ui.draw().addRectFilled(badge.deflated(1.0f),
                                needsBridge ? t.control.withAlpha(0.18f) : t.accent.withAlpha(0.14f),
                                2.0f);
        ui.draw().addTextClipped(ui.font(t.fontSmall), badge,
                                 needsBridge ? t.control : t.accent,
                                 plugin.architecture == vst2::Architecture::X86 ? "x86" : "x64",
                                 DrawList::Align::Centre);
        rowContent.removeFromRight(8.0f);

        if (plugin.isSynth) {
            const Rect synthBadge = rowContent.removeFromRight(46.0f);
            ui.draw().addTextClipped(ui.font(t.fontSmall), synthBadge, t.textFaint, "synth",
                                     DrawList::Align::Centre);
        }

        const Rect vendorArea = rowContent.removeFromRight(rowContent.width * 0.34f);
        ui.draw().addTextClipped(ui.font(t.fontSmall), vendorArea, t.textFaint, plugin.vendor);

        ui.draw().addTextClipped(ui.font(t.fontUi), rowContent,
                                 hovered || isSelected ? t.text : t.textDim, plugin.name);

        if (clicked) {
            selectedIndex_ = static_cast<int>(i);
            if (ui.input().mouseDoubleClicked[static_cast<int>(MouseButton::Left)] && onAddPlugin)
                onAddPlugin(plugin, forceBridge_);
        }

        if (hovered) ui.setTooltip(plugin.path);
    }

    if (showFailures_ && !failures.empty()) {
        content.removeFromTop(8.0f);
        ui.label(content.removeFromTop(18.0f), "could not be loaded", t.danger, t.fontUiBold);

        for (const vst2::FailedPlugin& failure : failures) {
            const Rect row = content.removeFromTop(19.0f);
            if (row.bottom() < area.top() || row.top() > area.bottom()) continue;

            Rect rowContent = row.deflated(3.0f);
            ui.draw().addTextClipped(ui.font(t.fontSmall),
                                     rowContent.removeFromLeft(rowContent.width * 0.4f),
                                     t.textDim, pathLeaf(failure.path));
            ui.draw().addTextClipped(ui.font(t.fontSmall), rowContent, t.danger.withAlpha(0.8f),
                                     failure.reason);
            if (ui.hovering(row)) ui.setTooltip(failure.path + "\n" + failure.reason);
        }
    }

    ui.endScroll();

    // -- add button --------------------------------------------------------
    if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(visible.size())) {
        const Rect addRow{ bounds.right() - 190.0f, bounds.bottom() - 44.0f, 174.0f, 30.0f };
        if (ui.button(ui.id("plugins.add"), addRow, "add to patch", Ui::ButtonStyle::Primary)) {
            if (onAddPlugin) onAddPlugin(*visible[static_cast<std::size_t>(selectedIndex_)], forceBridge_);
        }
    }
}

// ---------------------------------------------------------------------------
// TransportBar
// ---------------------------------------------------------------------------

void TransportBar::initialise(Engine* engine) { engine_ = engine; }

float TransportBar::height() { return 46.0f; }

void TransportBar::render(Ui& ui, const Rect& bounds, MainView& activeView) {
    if (!engine_) return;

    const Theme& t = theme();
    Transport& transport = engine_->transport();

    ui.draw().addRectFilledGradient(bounds, t.panelHeader, t.panel);
    ui.draw().addRectFilled(Rect{ bounds.left(), bounds.bottom() - 1.0f, bounds.width, 1.0f },
                            t.border);

    Rect area = bounds.deflated(t.smallPadding);
    area.removeFromLeft(4.0f);

    // -- patch actions -----------------------------------------------------
    const float buttonSize = area.height;

    if (ui.iconButton(ui.id("bar.new"), area.removeFromLeft(buttonSize), Ui::Icon::Plus, t.textDim))
        if (onNewPatch) onNewPatch();
    if (ui.isHot(ui.id("bar.new"))) ui.setTooltip("New patch  (Ctrl+N)");

    if (ui.iconButton(ui.id("bar.open"), area.removeFromLeft(buttonSize), Ui::Icon::Folder, t.textDim))
        if (onOpenPatch) onOpenPatch();
    if (ui.isHot(ui.id("bar.open"))) ui.setTooltip("Open patch  (Ctrl+O)");

    if (ui.iconButton(ui.id("bar.save"), area.removeFromLeft(buttonSize), Ui::Icon::Save, t.textDim))
        if (onSavePatch) onSavePatch();
    if (ui.isHot(ui.id("bar.save"))) ui.setTooltip("Save patch  (Ctrl+S)");

    area.removeFromLeft(10.0f);
    ui.separator(area.removeFromLeft(1.0f), true);
    area.removeFromLeft(10.0f);

    // -- transport ---------------------------------------------------------
    const bool playing = transport.playing();

    if (ui.iconButton(ui.id("bar.play"), area.removeFromLeft(buttonSize),
                      playing ? Ui::Icon::Pause : Ui::Icon::Play,
                      playing ? t.accent : t.text, playing))
        transport.togglePlaying();
    if (ui.isHot(ui.id("bar.play"))) ui.setTooltip("Play / pause  (Space)");

    if (ui.iconButton(ui.id("bar.stop"), area.removeFromLeft(buttonSize), Ui::Icon::Rewind, t.text)) {
        transport.setPlaying(false);
        transport.requestRewind();
    }
    if (ui.isHot(ui.id("bar.stop"))) ui.setTooltip("Stop and return to the start");

    const bool loop = transport.loopEnabled();
    if (ui.iconButton(ui.id("bar.loop"), area.removeFromLeft(buttonSize), Ui::Icon::Loop,
                      loop ? t.accent : t.textDim, loop))
        transport.setLoop(!loop, transport.loopStartPpq(), transport.loopEndPpq());

    area.removeFromLeft(10.0f);
    ui.separator(area.removeFromLeft(1.0f), true);
    area.removeFromLeft(10.0f);

    // -- tempo -------------------------------------------------------------
    const Rect bpmArea = area.removeFromLeft(72.0f);
    const UiId bpmField = ui.id("bar.bpm");

    if (!ui.editingText(bpmField)) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", transport.bpm());
        bpmBuffer_ = text;
    }

    if (ui.textField(bpmField, bpmArea, bpmBuffer_)) {
        const double parsed = std::strtod(bpmBuffer_.c_str(), nullptr);
        if (parsed >= 20.0 && parsed <= 999.0) transport.setBpm(parsed);
    }
    if (ui.isHot(bpmField)) ui.setTooltip("Tempo in beats per minute");
    area.removeFromLeft(4.0f);

    tapClock_ += static_cast<double>(ui.deltaSeconds());
    if (ui.button(ui.id("bar.tap"), area.removeFromLeft(42.0f), "tap")) {
        const double bpm = transport.tap(tapClock_);
        if (bpm > 0.0) {
            char message[64];
            std::snprintf(message, sizeof(message), "%.1f bpm", bpm);
            ui.notify(message, t.control, 1.5f);
        }
    }
    area.removeFromLeft(10.0f);

    // -- position readout --------------------------------------------------
    const TransportState state = transport.snapshot();
    const Rect positionArea = area.removeFromLeft(112.0f);

    ui.draw().addRectFilled(positionArea, t.panelSunken, t.cornerRadius);
    char position[64];
    std::snprintf(position, sizeof(position), "%3d . %d . %d/%d",
                  state.bar(), state.beatInBar(),
                  state.timeSigNumerator, state.timeSigDenominator);
    ui.draw().addTextClipped(ui.font(t.fontMono), positionArea, playing ? t.accent : t.textDim,
                             position, DrawList::Align::Centre);

    area.removeFromLeft(10.0f);
    ui.separator(area.removeFromLeft(1.0f), true);
    area.removeFromLeft(10.0f);

    // -- view tabs (right aligned) ----------------------------------------
    Rect tabArea = area.removeFromRight(300.0f);
    static const char* tabNames[] = { "patch", "metasurface", "plugins" };
    const float tabWidth = tabArea.width / 3.0f;

    for (int i = 0; i < 3; ++i) {
        const Rect tab = tabArea.removeFromLeft(tabWidth);
        const bool selected = static_cast<int>(activeView) == i;
        if (ui.button(ui.id(std::string("bar.tab.") + tabNames[i]), tab.deflated(2.0f),
                      tabNames[i], Ui::ButtonStyle::Toggle, selected))
            activeView = static_cast<MainView>(i);
    }

    area.removeFromRight(12.0f);

    // -- master section ----------------------------------------------------
    const Rect meterArea = area.removeFromRight(28.0f);
    ui.stereoMeter(meterArea.deflated(3.0f), engine_->masterPeak(0), engine_->masterPeak(1), false);
    area.removeFromRight(6.0f);

    const Rect masterArea = area.removeFromRight(130.0f);
    float masterNormalised = (engine_->masterGainDb() + 96.0f) / 108.0f;
    if (ui.sliderNormalised(ui.id("bar.master"), masterArea.deflated(6.0f), masterNormalised, t.accent))
        engine_->setMasterGainDb(masterNormalised * 108.0f - 96.0f);
    if (ui.isHot(ui.id("bar.master"))) {
        char text[48];
        std::snprintf(text, sizeof(text), "master %.1f dB", static_cast<double>(engine_->masterGainDb()));
        ui.setTooltip(text);
    }

    area.removeFromRight(6.0f);
    const Rect panicArea = area.removeFromRight(buttonSize);
    if (ui.iconButton(ui.id("bar.panic"), panicArea, Ui::Icon::Cross, t.danger))
        engine_->panic();
    if (ui.isHot(ui.id("bar.panic")))
        ui.setTooltip("Panic: silence and reset every node");

    // -- patch name (whatever is left in the middle) -----------------------
    if (area.width > 60.0f) {
        const std::string title = patchName_ + (modified_ ? " *" : "");
        ui.draw().addTextClipped(ui.font(t.fontUi), area, modified_ ? t.control : t.textDim,
                                 title, DrawList::Align::Centre);
    }
}

// ---------------------------------------------------------------------------
// StatusBar
// ---------------------------------------------------------------------------

void StatusBar::initialise(Engine* engine, vst2::PluginManager* plugins) {
    engine_ = engine;
    plugins_ = plugins;
}

float StatusBar::height() { return 22.0f; }

void StatusBar::render(Ui& ui, const Rect& bounds, const std::string& deviceDescription) {
    if (!engine_) return;

    const Theme& t = theme();
    ui.draw().addRectFilled(bounds, t.panelHeader);
    ui.draw().addRectFilled(Rect{ bounds.left(), bounds.top(), bounds.width, 1.0f }, t.border);

    Rect area = bounds.deflated(t.smallPadding);
    area.removeFromLeft(4.0f);

    const EngineStats stats = engine_->stats();

    // CPU first: it is the number that decides whether a set survives.
    const Rect cpuArea = area.removeFromLeft(150.0f);
    const float load = clampValue(stats.cpuLoad, 0.0f, 1.0f);
    const Colour loadColour = load > 0.85f ? t.danger : (load > 0.6f ? t.warning : t.textDim);

    const Rect cpuBar = Rect{ cpuArea.left(), cpuArea.centre().y - 3.0f, 52.0f, 6.0f };
    ui.draw().addRectFilled(cpuBar, t.widgetTrack, 1.5f);
    ui.draw().addRectFilled(Rect{ cpuBar.left(), cpuBar.top(), cpuBar.width * load, cpuBar.height },
                            loadColour, 1.5f);

    char cpuText[64];
    std::snprintf(cpuText, sizeof(cpuText), "%3.0f%% dsp", static_cast<double>(load * 100.0f));
    Rect cpuTextArea = cpuArea;
    cpuTextArea.removeFromLeft(58.0f);
    ui.draw().addTextClipped(ui.font(t.fontSmall), cpuTextArea, loadColour, cpuText);

    if (ui.hovering(cpuArea)) {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "peak %.0f%%, %d drop-outs since start",
                      static_cast<double>(stats.peakCpuLoad * 100.0f), stats.xruns);
        ui.setTooltip(detail);
    }

    ui.separator(area.removeFromLeft(1.0f), true);
    area.removeFromLeft(8.0f);

    // Graph shape.
    char graphText[128];
    std::snprintf(graphText, sizeof(graphText), "%d nodes", stats.nodeCount);
    ui.draw().addTextClipped(ui.font(t.fontSmall), area.removeFromLeft(72.0f), t.textFaint, graphText);

    if (stats.feedbackEdges > 0) {
        char feedbackText[96];
        std::snprintf(feedbackText, sizeof(feedbackText), "%d feedback %s", stats.feedbackEdges,
                      stats.feedbackEdges == 1 ? "edge" : "edges");
        ui.draw().addTextClipped(ui.font(t.fontSmall), area.removeFromLeft(120.0f),
                                 t.cableFeedback, feedbackText);
        if (ui.hovering(area)) ui.setTooltip("Feedback edges add one block of latency each");
    }

    if (stats.xruns > 0) {
        char xrunText[64];
        std::snprintf(xrunText, sizeof(xrunText), "%d drop-outs", stats.xruns);
        ui.draw().addTextClipped(ui.font(t.fontSmall), area.removeFromLeft(110.0f),
                                 t.danger, xrunText);
    }

    // Plugin count, right of centre.
    if (plugins_) {
        const std::size_t count = plugins_->plugins().size();
        char pluginText[64];
        std::snprintf(pluginText, sizeof(pluginText), "%zu plugins", count);
        ui.draw().addTextClipped(ui.font(t.fontSmall), area.removeFromRight(96.0f),
                                 t.textFaint, pluginText, DrawList::Align::Right);
    }

    // Device description on the right: sample rate, block size, latency.
    ui.draw().addTextClipped(ui.font(t.fontSmall), area, t.textFaint, deviceDescription,
                             DrawList::Align::Right);
}

} // namespace acm::ui
