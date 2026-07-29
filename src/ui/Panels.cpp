#include "Panels.h"

#include "../audio/AudioFile.h"
#include "../core/AppPaths.h"
#include "../core/FileIo.h"
#include "../patch/Patch.h"
#include "../nodes/BuildNode.h"
#include "../nodes/ColorNode.h"
#include "../nodes/StemPlayerNode.h"
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
    // Ours first, because that is where saved work goes, then the folders people
    // actually keep samples in, then every mounted volume. A browser that can
    // only see the application's own directories cannot reach a sample library,
    // which makes it look like there is no browser at all.
    const auto add = [this](std::string name, std::string path) {
        if (!path.empty()) places_.push_back({ std::move(name), std::move(path) });
    };

    add("patches", paths::patchesDirectory());
    add("recordings", paths::recordingsDirectory());
    add("home", paths::userProfile());
    add("desktop", paths::desktop());
    add("downloads", paths::downloads());
    add("music", paths::musicFolder());
    add("documents", paths::documents());

    for (const std::string& root : paths::driveRoots())
        add(root.substr(0, 2), root);   // "C:\" reads better as "C:"

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
    const Theme& t = theme();
    const float rowHeight = 19.0f;
    const float gap = 3.0f;

    // Chips wrap onto as many rows as they need. A single row of fixed-width
    // buttons silently dropped everything past the fourth place once drives were
    // added, which is exactly the sort of thing nobody notices is missing.
    Rect row = area.removeFromTop(rowHeight);
    for (const Place& place : places_) {
        const float width = std::min(ui.font(t.fontUi).textWidth(place.name)
                                         + t.smallPadding * 2.0f + 8.0f,
                                     area.width);

        if (width > row.width) {
            area.removeFromTop(gap);
            row = area.removeFromTop(rowHeight);
        }

        const Rect chip = row.removeFromLeft(width);
        row.removeFromLeft(gap);

        const bool here = place.path == currentDirectory_;
        if (ui.button(ui.id("browser.place." + place.path), chip, place.name,
                      Ui::ButtonStyle::Toggle, here))
            navigateTo(place.path);
        if (ui.isHot(ui.id("browser.place." + place.path))) ui.setTooltip(place.path);
    }

    area.removeFromTop(6.0f);
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

    const Rect refreshButton = pathRow.removeFromRight(24.0f);
    if (ui.iconButton(ui.id("browser.refresh"), refreshButton, Ui::Icon::Refresh, t.textDim))
        needsRefresh_ = true;
    if (ui.isHot(ui.id("browser.refresh"))) ui.setTooltip("Re-read this folder");
    pathRow.removeFromRight(4.0f);

    // Editable, so a path can be pasted straight in. Navigating by clicking
    // through from a drive root is a lot of clicks when you already know where
    // the library lives.
    if (pathBufferFor_ != currentDirectory_) {
        pathBuffer_ = currentDirectory_;
        pathBufferFor_ = currentDirectory_;
    }
    if (ui.textField(ui.id("browser.path"), pathRow, pathBuffer_, "path")) {
        const std::string typed = pathNormalise(pathBuffer_);
        if (!typed.empty() && typed != currentDirectory_) navigateTo(typed);
    }
    if (ui.isHot(ui.id("browser.path"))) ui.setTooltip(currentDirectory_);

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

    // An empty panel is indistinguishable from a broken one, so say which it is.
    if (visible.empty()) {
        ui.draw().addTextClipped(ui.font(t.fontSmall), content.removeFromTop(40.0f), t.textFaint,
                                 entries_.empty() ? "no audio or patch files here"
                                                  : "nothing matches that filter",
                                 DrawList::Align::Centre);
    }

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


void InspectorView::drawStemSection(Ui& ui, Rect& area, Node& node) {
    auto* stems = dynamic_cast<StemPlayerNode*>(&node);
    if (!stems) return;

    const Theme& t = theme();

    ui.separator(area.removeFromTop(9.0f));
    Rect header = area.removeFromTop(18.0f);
    ui.label(header.removeFromLeft(90.0f), "sections", t.textDim, t.fontUiBold);

    if (ui.button(ui.id("inspector.section.add"), header.removeFromRight(52.0f), "add",
                  Ui::ButtonStyle::Normal, false, stems->sectionCount() < kMaxSections)) {
        // A new section starts where the last one ends, which is almost always
        // what is wanted when marking a song up in order.
        StemSection section;
        if (stems->sectionCount() > 0) {
            const StemSection& last = stems->sections().back();
            section.startBar = last.startBar + last.lengthBars;
            section.lengthBars = last.lengthBars;
        }
        section.name = "section " + std::to_string(stems->sectionCount() + 1);
        section.hue = static_cast<float>(stems->sectionCount()) * 0.13f;
        stems->addSection(section);
    }

    area.removeFromTop(3.0f);

    for (int i = 0; i < stems->sectionCount(); ++i) {
        if (area.height < 60.0f) break;

        const StemSection& section = stems->sections()[static_cast<std::size_t>(i)];
        StemSection edited = section;

        Rect row = area.removeFromTop(20.0f);
        const bool isActive = i == stems->activeSection();

        if (isActive)
            ui.draw().addRectFilled(row, t.accent.withAlpha(0.10f), t.cornerRadius);

        const Rect removeArea = row.removeFromRight(20.0f);
        if (ui.iconButton(ui.idFrom(&node, 700 + i), removeArea, Ui::Icon::Cross, t.textFaint)) {
            stems->removeSection(i);
            break;   // the list moved underneath us
        }

        const Rect playArea = row.removeFromRight(22.0f);
        if (ui.iconButton(ui.idFrom(&node, 730 + i), playArea, Ui::Icon::Play,
                          isActive ? t.accent : t.textDim))
            stems->requestSection(i);

        // Name, editable in place.
        if (editingSection_ != i) {
            bool hovered = false, held = false;
            if (ui.buttonBehaviour(ui.idFrom(&node, 760 + i), row, hovered, held)) {
                editingSection_ = i;
                sectionNameBuffer_ = section.name;
            }
            ui.draw().addTextClipped(ui.font(t.fontUi), row, isActive ? t.text : t.textDim,
                                     section.name);
        } else {
            if (ui.textField(ui.idFrom(&node, 790 + i), row, sectionNameBuffer_)) {
                edited.name = sectionNameBuffer_;
                stems->updateSection(i, edited);
                editingSection_ = -1;
            }
        }

        // Bars.
        Rect barRow = area.removeFromTop(18.0f);
        ui.label(barRow.removeFromLeft(28.0f), "bar", t.textFaint, t.fontSmall);

        int startBar = section.startBar;
        if (ui.intField(ui.idFrom(&node, 820 + i), barRow.removeFromLeft(52.0f), startBar, 0, 4096)) {
            edited.startBar = startBar;
            stems->updateSection(i, edited);
        }

        barRow.removeFromLeft(6.0f);
        ui.label(barRow.removeFromLeft(30.0f), "len", t.textFaint, t.fontSmall);

        int lengthBars = section.lengthBars;
        if (ui.intField(ui.idFrom(&node, 850 + i), barRow.removeFromLeft(52.0f), lengthBars, 1, 512)) {
            edited.lengthBars = lengthBars;
            stems->updateSection(i, edited);
        }

        area.removeFromTop(3.0f);
    }

    if (stems->sectionCount() == 0) {
        ui.draw().addTextClipped(ui.font(t.fontSmall), area.removeFromTop(28.0f), t.textFaint,
                                 "no sections yet - the whole song loops");
    }
}

void InspectorView::drawColorSection(Ui& ui, Rect& area, Node& node) {
    auto* color = dynamic_cast<ColorNode*>(&node);
    if (!color || !engine_) return;

    const Theme& t = theme();

    ui.separator(area.removeFromTop(9.0f));
    Rect header = area.removeFromTop(18.0f);
    ui.label(header.removeFromLeft(90.0f), "colour targets", t.textDim, t.fontUiBold);

    char count[48];
    std::snprintf(count, sizeof(count), "%d", static_cast<int>(color->targets().size()));
    ui.label(header, count, t.textFaint, t.fontSmall, DrawList::Align::Right);

    area.removeFromTop(3.0f);

    // Adopting a whole node is how a chain gets set up: pick the plugin, take
    // every parameter it has, then capture the two ends by ear and prune.
    Rect adoptRow = area.removeFromTop(20.0f);
    ui.label(adoptRow.removeFromLeft(46.0f), "adopt", t.textFaint, t.fontSmall);

    std::vector<std::string> names;
    std::vector<NodeId> ids;
    for (const auto& candidate : engine_->graph().nodes()) {
        if (candidate->id() == node.id()) continue;
        if (candidate->numParameters() == 0) continue;
        names.push_back(candidate->name());
        ids.push_back(candidate->id());
    }

    if (names.empty()) {
        ui.label(adoptRow, "nothing to drive yet", t.textFaint, t.fontSmall);
    } else {
        int chosen = -1;
        if (ui.combo(ui.id("inspector.color.adopt"), adoptRow, names, chosen)
            && chosen >= 0 && chosen < static_cast<int>(ids.size())) {
            const int added = color->adoptNode(ids[static_cast<std::size_t>(chosen)],
                                               engine_->graph());
            ui.notify(std::to_string(added) + " parameters added", t.accent, 2.0f);
        }
    }

    area.removeFromTop(3.0f);

    for (int i = 0; i < static_cast<int>(color->targets().size()); ++i) {
        if (area.height < 40.0f) break;

        const ColorTarget& target = color->targets()[static_cast<std::size_t>(i)];
        Rect row = area.removeFromTop(17.0f);

        const Rect removeArea = row.removeFromRight(18.0f);
        if (ui.iconButton(ui.idFrom(&node, 900 + i), removeArea, Ui::Icon::Cross, t.textFaint)) {
            color->removeTarget(i);
            break;
        }

        // How far this target actually travels, drawn as a bar - a target whose
        // ends are identical does nothing, and that should be visible without
        // opening it.
        const Rect travelArea = row.removeFromRight(40.0f).deflated(2.0f);
        const float travel = std::max(std::abs(target.redValue - target.neutralValue),
                                      std::abs(target.blueValue - target.neutralValue));
        ui.draw().addRectFilled(travelArea, t.widgetTrack, 2.0f);
        if (travel > 0.001f) {
            ui.draw().addRectFilled(Rect{ travelArea.left(), travelArea.top(),
                                          travelArea.width * clampValue(travel, 0.0f, 1.0f),
                                          travelArea.height }, t.accentDim, 2.0f);
        }

        const std::string label = target.nodeName + " / " + target.paramName;
        ui.draw().addTextClipped(ui.font(t.fontSmall), row,
                                 travel > 0.001f ? t.textDim : t.textFaint, label);
        if (ui.hovering(row)) ui.setTooltip(label);
    }
}

void InspectorView::drawBuildSection(Ui& ui, Rect& area, Node& node) {
    auto* build = dynamic_cast<BuildNode*>(&node);
    if (!build || !engine_) return;

    const Theme& t = theme();

    ui.separator(area.removeFromTop(9.0f));
    ui.label(area.removeFromTop(18.0f), "build targets", t.textDim, t.fontUiBold);
    area.removeFromTop(3.0f);

    // Two pickers, one per kind of target. Listing only the nodes that can
    // actually be driven means a wrong choice is not possible.
    const auto picker = [&](const char* caption, const char* typeName, NodeId current,
                            const std::function<void(NodeId)>& assign) {
        Rect row = area.removeFromTop(20.0f);
        ui.label(row.removeFromLeft(52.0f), caption, t.textFaint, t.fontSmall);

        std::vector<std::string> names{ "none" };
        std::vector<NodeId> ids{ kInvalidNode };
        int selected = 0;

        for (const auto& candidate : engine_->graph().nodes()) {
            if (candidate->typeName() != typeName) continue;
            if (candidate->id() == current) selected = static_cast<int>(ids.size());
            names.push_back(candidate->name());
            ids.push_back(candidate->id());
        }

        if (ui.combo(ui.idFrom(&node, 950 + static_cast<int>(names.size())), row, names, selected)
            && selected >= 0 && selected < static_cast<int>(ids.size()))
            assign(ids[static_cast<std::size_t>(selected)]);

        area.removeFromTop(3.0f);
    };

    picker("stems", "stem.player", build->stemPlayer(),
           [build](NodeId id) { build->setStemPlayer(id); });
    picker("colour", "color", build->colorNode(),
           [build](NodeId id) { build->setColorNode(id); });

    Rect riserRow = area.removeFromTop(18.0f);
    ui.label(riserRow.removeFromLeft(52.0f), "riser", t.textFaint, t.fontSmall);
    ui.draw().addTextClipped(ui.font(t.fontSmall), riserRow,
                             build->riserPath().empty() ? t.textFaint : t.textDim,
                             build->riserPath().empty() ? "drop an audio file on the node"
                                                        : pathLeaf(build->riserPath()));
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
    drawStemSection(ui, area, *node);
    drawColorSection(ui, area, *node);
    drawBuildSection(ui, area, *node);

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

    // Carve the action strip out of the panel before the scroller claims what is
    // left. Floating it over the list would put a row underneath every button,
    // and because the rows are processed first they would take the press and the
    // buttons would never fire.
    Rect actionRow = area.removeFromBottom(38.0f);
    actionRow.removeFromTop(8.0f);

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

        const bool isSelected = plugin.path == selectedPath_;
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
            selectedPath_ = plugin.path;
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

    // -- action strip ------------------------------------------------------
    // Resolved from the list rather than remembered as a pointer: `plugins` is a
    // fresh copy each frame, so anything held across frames would dangle.
    const vst2::PluginDescription* selected = nullptr;
    for (const vst2::PluginDescription* candidate : visible)
        if (candidate->path == selectedPath_) { selected = candidate; break; }

    const Rect addRect = actionRow.removeFromRight(174.0f);
    if (ui.button(ui.id("plugins.add"), addRect, "add to patch",
                  Ui::ButtonStyle::Primary, false, selected != nullptr) && selected) {
        if (onAddPlugin) onAddPlugin(*selected, forceBridge_);
    }

    actionRow.removeFromRight(10.0f);
    ui.draw().addTextClipped(ui.font(t.fontSmall), actionRow, t.textFaint,
                             selected ? selected->name + "  -  " + selected->vendor
                                      : "select a plugin, or double-click one to add it",
                             DrawList::Align::Right);
}


// ---------------------------------------------------------------------------
// SettingsView
// ---------------------------------------------------------------------------

void SettingsView::initialise(Engine* engine, platform::AudioDeviceSettings* settings) {
    engine_ = engine;
    settings_ = settings;
}

void SettingsView::close() {
    visible_ = false;
    if (engine_) engine_->stopOutputTest();
}

void SettingsView::open() {
    visible_ = true;
    justOpened_ = true;
    dirty_ = false;
    if (settings_) draft_ = *settings_;
    // Enumerating endpoints touches COM and can take a moment on a machine with
    // a lot of interfaces, so it happens on open rather than every frame.
    refreshDeviceLists();
}

void SettingsView::refreshDeviceLists() {
    asioAvailable_ = platform::audioBackendAvailable(platform::AudioBackend::Asio);

    outputDevices_ = platform::outputDevices(draft_.backend);
    inputDevices_ = platform::inputDevices(draft_.backend);

    // The channel count is the thing worth knowing when picking between two
    // interfaces from the same manufacturer, and it is why the ASIO list is
    // worth the cost of opening each driver to ask.
    const auto describe = [](const platform::AudioDeviceInfo& device) {
        std::string text = device.name;
        if (device.outputChannels > 0) text += "  (" + std::to_string(device.outputChannels) + " out)";
        if (device.isDefault) text += "  (default)";
        return text;
    };

    outputNames_.clear();
    for (const auto& device : outputDevices_) outputNames_.push_back(describe(device));

    inputNames_.clear();
    inputNames_.push_back("none");
    for (const auto& device : inputDevices_) inputNames_.push_back(describe(device));

    deviceListsLoaded_ = true;
}

bool SettingsView::render(Ui& ui, const Rect& bounds, const platform::AudioDeviceStatus& status) {
    if (!visible_ || !settings_) return false;

    const Theme& t = theme();
    DrawList& list = ui.draw();

    // Everything from here on is inside the modal, so it is the only thing the
    // pointer can reach. The application put the Ui into modal mode before any
    // of the views underneath ran; this is the hole punched in it.
    ui.beginModal();
    struct EndModal {
        Ui& ui;
        ~EndModal() { ui.endModal(); }
    } endModal{ ui };

    // Scrim.
    list.addRectFilled(bounds, Colour{ 0.0f, 0.0f, 0.0f, 0.55f });

    const float width = std::min(t.scaled(520.0f), bounds.width - t.scaled(40.0f));
    const float height = std::min(t.scaled(516.0f), bounds.height - t.scaled(40.0f));
    const Rect sheet{ bounds.centre().x - width * 0.5f, bounds.centre().y - height * 0.5f,
                      width, height };

    // Dismiss on a click outside the sheet - but never on the very press that
    // opened it. The button that opens the panel is itself outside the sheet,
    // and when a press and its release land in the same frame the panel would
    // otherwise open and close without ever being drawn.
    if (!justOpened_
        && ui.input().mousePressed[static_cast<int>(MouseButton::Left)]
        && !sheet.contains(ui.input().mousePosition)) {
        close();
        return true;
    }
    justOpened_ = false;

    list.addRectFilled(sheet.translated({ 0.0f, 4.0f }), Colour{ 0.0f, 0.0f, 0.0f, 0.5f },
                       t.cornerRadiusLarge);
    list.addRectFilled(sheet, t.panelRaised, t.cornerRadiusLarge);
    list.addRect(sheet, t.borderStrong, t.borderWidth, t.cornerRadiusLarge);

    Rect area = sheet.deflated(t.padding * 1.5f);

    // -- title -------------------------------------------------------------
    Rect titleRow = area.removeFromTop(t.scaled(28.0f));
    const Rect closeArea = titleRow.removeFromRight(t.scaled(24.0f));
    ui.label(titleRow, "settings", t.text, t.fontTitle);
    if (ui.iconButton(ui.id("settings.close"), closeArea, Ui::Icon::Cross, t.textDim)) {
        close();
        return true;
    }
    area.removeFromTop(t.scaled(6.0f));
    ui.separator(area.removeFromTop(t.scaled(9.0f)));

    const float rowHeight = t.scaled(26.0f);
    const float labelWidth = t.scaled(140.0f);
    const auto labelled = [&](Rect& region, const char* caption) {
        Rect row = region.removeFromTop(rowHeight);
        region.removeFromTop(t.scaled(6.0f));
        ui.label(row.removeFromLeft(labelWidth), caption, t.textDim, t.fontUi);
        return row;
    };

    // -- audio output ------------------------------------------------------
    ui.label(area.removeFromTop(t.scaled(18.0f)), "audio output", t.accent, t.fontUiBold);
    area.removeFromTop(t.scaled(4.0f));

    {
        // The driver model comes first because it decides what everything below
        // it can offer. WASAPI shared mode is stuck with the endpoint's mix
        // format, which is stereo on nearly every interface no matter how many
        // outputs it has; ASIO is what reaches the rest of them.
        Rect row = labelled(area, "driver");

        const Rect backendArea = row.removeFromLeft(row.width * 0.46f);
        row.removeFromLeft(t.scaled(8.0f));

        static const std::vector<std::string> backends = { "WASAPI (shared)", "ASIO" };
        int backendIndex = draft_.backend == platform::AudioBackend::Asio ? 1 : 0;

        if (ui.combo(ui.id("settings.backend"), backendArea, backends, backendIndex)) {
            const auto chosen = backendIndex == 1 ? platform::AudioBackend::Asio
                                                  : platform::AudioBackend::Wasapi;
            if (chosen != draft_.backend) {
                draft_.backend = chosen;
                // Device ids do not carry across backends, and neither does a
                // routing offset chosen against a different channel count.
                draft_.outputDeviceId.clear();
                draft_.inputDeviceId.clear();
                draft_.outputChannelCount = 0;
                draft_.outputChannelOffset = 0;
                refreshDeviceLists();
                dirty_ = true;
            }
        }

        if (draft_.backend == platform::AudioBackend::Asio && !asioAvailable_) {
            ui.label(row, "no ASIO driver installed", t.danger, t.fontSmall);
        } else if (onShowControlPanel && draft_.backend == platform::AudioBackend::Asio) {
            // The driver's own window is the only place its buffer size and
            // clock source can be set, so there has to be a way to reach it.
            if (ui.button(ui.id("settings.asiopanel"), row, "driver control panel",
                          Ui::ButtonStyle::Normal, false, controlPanelAvailable_))
                onShowControlPanel();
            if (ui.isHot(ui.id("settings.asiopanel")) && !controlPanelAvailable_)
                ui.setTooltip("Available once the ASIO driver is open. Apply first.");
        }
    }

    {
        Rect row = labelled(area, "device");

        int selected = 0;
        for (std::size_t i = 0; i < outputDevices_.size(); ++i) {
            if (outputDevices_[i].id == draft_.outputDeviceId) { selected = static_cast<int>(i); break; }
            if (draft_.outputDeviceId.empty() && outputDevices_[i].isDefault) selected = static_cast<int>(i);
        }

        if (outputNames_.empty()) {
            ui.label(row, "no output devices found", t.danger, t.fontUi);
        } else if (ui.combo(ui.id("settings.outdev"), row, outputNames_, selected)) {
            draft_.outputDeviceId = outputDevices_[static_cast<std::size_t>(selected)].id;
            // A different endpoint has its own channel count, so the routing
            // offset from the previous one is meaningless.
            draft_.outputChannelOffset = 0;
            dirty_ = true;
        }
    }

    {
        // Channel count and first channel: the pair that decides where a stereo
        // patch lands on a multi-output interface.
        //
        // Taken from the device the user has *selected*, not the one currently
        // running. Reading it from the live status meant a 20-output interface
        // still offered two channels until after it had been applied - and
        // applying is exactly what you cannot judge without setting the routing
        // first. Enumeration reports the count for every device, so use it, and
        // fall back to the running device only when it does not.
        int deviceChannels = 0;
        for (const platform::AudioDeviceInfo& device : outputDevices_) {
            const bool isSelected = device.id == draft_.outputDeviceId
                                 || (draft_.outputDeviceId.empty() && device.isDefault);
            if (isSelected && device.outputChannels > 0) {
                deviceChannels = device.outputChannels;
                break;
            }
        }
        if (deviceChannels <= 0) deviceChannels = status.deviceOutputChannels;
        deviceChannels = std::max(2, deviceChannels);

        Rect row = labelled(area, "channels");
        const Rect countArea = row.removeFromLeft(row.width * 0.46f);
        row.removeFromLeft(t.scaled(8.0f));

        std::vector<std::string> counts;
        for (int c = 1; c <= deviceChannels; ++c)
            counts.push_back(std::to_string(c) + (c == 1 ? " channel" : " channels"));

        int countIndex = (draft_.outputChannelCount > 0 ? draft_.outputChannelCount : deviceChannels) - 1;
        countIndex = clampValue(countIndex, 0, static_cast<int>(counts.size()) - 1);

        if (ui.combo(ui.id("settings.outcount"), countArea, counts, countIndex)) {
            draft_.outputChannelCount = countIndex + 1;
            const int maximumOffset = std::max(0, deviceChannels - draft_.outputChannelCount);
            draft_.outputChannelOffset = clampValue(draft_.outputChannelOffset, 0, maximumOffset);
            dirty_ = true;
        }

        const int busChannels = draft_.outputChannelCount > 0 ? draft_.outputChannelCount : deviceChannels;
        std::vector<std::string> offsets;
        for (int c = 0; c + busChannels <= deviceChannels; ++c) {
            offsets.push_back(busChannels == 1
                ? ("out " + std::to_string(c + 1))
                : ("out " + std::to_string(c + 1) + "-" + std::to_string(c + busChannels)));
        }
        if (offsets.empty()) offsets.push_back("out 1");

        int offsetIndex = clampValue(draft_.outputChannelOffset, 0, static_cast<int>(offsets.size()) - 1);
        if (ui.combo(ui.id("settings.outoffset"), row, offsets, offsetIndex)) {
            draft_.outputChannelOffset = offsetIndex;
            dirty_ = true;
        }
        if (ui.isHot(ui.id("settings.outoffset")))
            ui.setTooltip("Which of the device's outputs the master bus is sent to");

        // -- identify ------------------------------------------------------
        // Walks a blip across the chosen outputs. Knowing which socket is which
        // is the whole reason for setting an offset, and on a twenty-output
        // interface it is not something you can work out by reading a manual.
        Rect testRow = labelled(area, "identify");
        const Rect testButton = testRow.removeFromLeft(t.scaled(110.0f));
        testRow.removeFromLeft(t.scaled(10.0f));

        const bool testing = engine_ && engine_->outputTestRunning();
        if (ui.button(ui.id("settings.outtest"), testButton, testing ? "stop" : "test outputs",
                      testing ? Ui::ButtonStyle::Danger : Ui::ButtonStyle::Normal,
                      false, engine_ != nullptr && status.running)) {
            if (testing) engine_->stopOutputTest();
            // The applied routing, not the draft: the tone goes to the device
            // that is actually open, and saying otherwise would be a lie about
            // which socket is about to make a noise.
            else engine_->startOutputTest(settings_->outputChannelOffset,
                                          status.outputChannels > 0 ? status.outputChannels
                                                                    : busChannels);
        }

        if (!status.running) {
            ui.draw().addTextClipped(ui.font(t.fontSmall), testRow, t.textFaint,
                                     "starts once a device is open");
        } else if (testing) {
            char text[96];
            std::snprintf(text, sizeof(text), "sounding output %d",
                          engine_->outputTestChannel() + 1);
            ui.draw().addTextClipped(ui.font(t.fontSmall), testRow, t.accent, text);
        } else {
            char text[128];
            std::snprintf(text, sizeof(text), "a blip on each of outputs %d-%d, in turn",
                          settings_->outputChannelOffset + 1,
                          settings_->outputChannelOffset
                              + (status.outputChannels > 0 ? status.outputChannels : busChannels));
            ui.draw().addTextClipped(ui.font(t.fontSmall), testRow, t.textFaint, text);
        }
    }

    area.removeFromTop(t.scaled(4.0f));
    ui.separator(area.removeFromTop(t.scaled(9.0f)));

    // -- audio input -------------------------------------------------------
    ui.label(area.removeFromTop(t.scaled(18.0f)), "audio input", t.accent, t.fontUiBold);
    area.removeFromTop(t.scaled(4.0f));

    {
        Rect row = labelled(area, "device");

        int selected = 0;   // 0 = none
        if (draft_.enableInput) {
            for (std::size_t i = 0; i < inputDevices_.size(); ++i) {
                if (inputDevices_[i].id == draft_.inputDeviceId) { selected = static_cast<int>(i) + 1; break; }
                if (draft_.inputDeviceId.empty() && inputDevices_[i].isDefault) selected = static_cast<int>(i) + 1;
            }
        }

        if (ui.combo(ui.id("settings.indev"), row, inputNames_, selected)) {
            draft_.enableInput = selected > 0;
            draft_.inputDeviceId = selected > 0
                ? inputDevices_[static_cast<std::size_t>(selected - 1)].id
                : std::string();
            dirty_ = true;
        }
    }

    area.removeFromTop(t.scaled(4.0f));
    ui.separator(area.removeFromTop(t.scaled(9.0f)));

    // -- buffer ------------------------------------------------------------
    ui.label(area.removeFromTop(t.scaled(18.0f)), "buffer", t.accent, t.fontUiBold);
    area.removeFromTop(t.scaled(4.0f));

    {
        Rect row = labelled(area, "size");

        static const std::vector<int> sizes = { 64, 128, 256, 512, 1024, 2048 };
        std::vector<std::string> labels;
        for (int size : sizes) {
            // Latency is the number that actually matters to a performer.
            const double milliseconds = status.sampleRate > 0.0
                ? 1000.0 * size / status.sampleRate : 0.0;
            char text[64];
            std::snprintf(text, sizeof(text), "%d frames  (%.1f ms)", size, milliseconds);
            labels.push_back(text);
        }

        int index = 2;
        for (std::size_t i = 0; i < sizes.size(); ++i)
            if (sizes[i] == draft_.blockSize) index = static_cast<int>(i);

        if (ui.combo(ui.id("settings.buffer"), row, labels, index)) {
            draft_.blockSize = sizes[static_cast<std::size_t>(index)];
            dirty_ = true;
        }
    }

    // -- current state -----------------------------------------------------
    area.removeFromTop(t.scaled(6.0f));
    Rect statusRow = area.removeFromTop(t.scaled(34.0f));
    list.addRectFilled(statusRow, t.panelSunken, t.cornerRadius);

    const std::string summary = status.running
        ? (status.outputDeviceName + "  -  " + std::to_string(static_cast<int>(status.sampleRate))
           + " Hz, " + std::to_string(status.blockSize) + " frames")
        : std::string("not running: ") + (status.error.empty() ? "no device" : status.error);

    list.addTextClipped(ui.font(t.fontSmall), statusRow.deflated(t.scaled(8.0f)),
                        status.running ? t.textDim : t.danger, summary);

    // -- master ------------------------------------------------------------
    area.removeFromTop(t.scaled(8.0f));
    if (engine_) {
        bool limiter = engine_->masterLimiterEnabled();
        if (ui.checkbox(ui.id("settings.limiter"), area.removeFromTop(rowHeight),
                        "master limiter", limiter))
            engine_->setMasterLimiterEnabled(limiter);
        if (ui.isHot(ui.id("settings.limiter")))
            ui.setTooltip("A soft ceiling on the master bus. Leave this on unless you "
                          "know why you want it off.");
    }

    // -- actions -----------------------------------------------------------
    Rect actions = area.removeFromBottom(t.scaled(30.0f));
    const Rect applyArea = actions.removeFromRight(t.scaled(110.0f));
    actions.removeFromRight(t.scaled(8.0f));
    const Rect cancelArea = actions.removeFromRight(t.scaled(90.0f));

    if (ui.button(ui.id("settings.cancel"), cancelArea, "close")) {
        close();
        return true;
    }

    if (ui.button(ui.id("settings.apply"), applyArea, "apply",
                  Ui::ButtonStyle::Primary, false, dirty_)) {
        *settings_ = draft_;
        dirty_ = false;
        if (onApplyAudioSettings) onApplyAudioSettings();
    }

    if (dirty_) {
        ui.label(actions, "restarts the audio device", t.warning, t.fontSmall,
                 DrawList::Align::Right);
    }

    return true;
}

// ---------------------------------------------------------------------------
// TransportBar
// ---------------------------------------------------------------------------

void TransportBar::initialise(Engine* engine) { engine_ = engine; }

float TransportBar::height() { return theme().scaled(46.0f); }

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

    if (showSettingsButton
        && ui.iconButton(ui.id("bar.settings"), area.removeFromLeft(buttonSize),
                         Ui::Icon::Gear, t.textDim))
        if (onOpenSettings) onOpenSettings();
    if (ui.isHot(ui.id("bar.settings"))) ui.setTooltip("Audio settings  (Ctrl+,)");

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

float StatusBar::height() { return theme().scaled(22.0f); }

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
