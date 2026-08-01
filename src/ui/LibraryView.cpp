#include "LibraryView.h"

#include "../core/FileIo.h"

#include <algorithm>
#include <cstdio>

namespace acm::ui {
namespace {

float listWidth() { return theme().scaled(240.0f); }
float rowHeight() { return theme().scaled(22.0f); }

// Splits a stored block of text into the lines the editor works on. A trailing
// newline does not become an empty line, because nobody typed it.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') { lines.push_back(current); current.clear(); }
        else if (c != '\r') { current.push_back(c); }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

Colour tagColour(const library::Tag& tag) {
    return Colour{ static_cast<float>((tag.colour >> 16) & 0xFF) / 255.0f,
                   static_cast<float>((tag.colour >> 8) & 0xFF) / 255.0f,
                   static_cast<float>(tag.colour & 0xFF) / 255.0f, 1.0f };
}

} // namespace

void LibraryView::initialise(Engine* engine, library::Library* library,
                             library::EntryKind kind) {
    engine_ = engine;
    library_ = library;
    kind_ = kind;
}

library::Entry* LibraryView::selectedEntry() {
    if (!library_ || selected_.empty()) return nullptr;
    return library_->find(selected_);
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

void LibraryView::drawList(Ui& ui, const Rect& area) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(area, t.panel);
    list.addRectFilled(Rect{ area.right() - 1.0f, area.top(), 1.0f, area.height }, t.border);

    Rect column = area.deflated(t.smallPadding);

    const bool projects = kind_ == library::EntryKind::Project;

    Rect header = column.removeFromTop(t.scaled(22.0f));
    ui.label(header.removeFromLeft(t.scaled(70.0f)), projects ? "projects" : "songs",
             t.textDim, t.fontUiBold);

    if (ui.button(ui.id("library.new"), header.removeFromRight(t.scaled(46.0f)), "new")) {
        selected_ = library_->create(kind_, projects ? "new project" : "new song");
        bufferFor_.clear();
    }

    column.removeFromTop(t.scaled(4.0f));
    ui.textField(ui.id("library.filter"), column.removeFromTop(t.scaled(22.0f)),
                 filter_, "filter");
    column.removeFromTop(t.scaled(6.0f));

    const std::vector<const library::Entry*> entries =
        filter_.empty() ? library_->entriesOfKind(kind_) : library_->search(filter_, kind_);

    if (entries.empty()) {
        ui.labelDim(column.removeFromTop(t.scaled(40.0f)),
                    filter_.empty() ? "nothing here yet - press new" : "nothing matches");
        return;
    }

    const Rect listArea = column;
    Rect content = ui.beginScroll(ui.id("library.list"), listArea,
                                  static_cast<float>(entries.size()) * rowHeight());

    for (const library::Entry* entry : entries) {
        Rect row = content.removeFromTop(rowHeight());
        if (row.bottom() < listArea.top() || row.top() > listArea.bottom()) continue;

        const bool isSelected = entry->id == selected_;
        bool hovered = false, held = false;
        if (ui.buttonBehaviour(ui.id("library.row." + entry->id), row, hovered, held)) {
            selected_ = entry->id;
            bufferFor_.clear();
            editingLine_ = -1;
        }

        if (isSelected) list.addRectFilled(row, t.selection.withAlpha(0.20f), 2.0f);
        else if (hovered) list.addRectFilled(row, t.widgetHover, 2.0f);

        Rect text = row.deflated(t.scaled(4.0f));

        // The count of what is inside says more at a glance than the name alone:
        // an album with no songs in it looks like one that has them.
        char suffix[48];
        if (projects) {
            std::snprintf(suffix, sizeof(suffix), "%d", static_cast<int>(entry->members.size()));
        } else {
            std::snprintf(suffix, sizeof(suffix), "%d", static_cast<int>(entry->files.size()));
        }
        list.addTextClipped(ui.font(t.fontSmall), text.removeFromRight(t.scaled(24.0f)),
                            t.textFaint, suffix, DrawList::Align::Right);

        list.addTextClipped(ui.font(t.fontUi), text, isSelected ? t.text : t.textDim,
                            entry->name);
    }

    ui.endScroll();
}

// ---------------------------------------------------------------------------
// The detail pane
// ---------------------------------------------------------------------------

void LibraryView::drawHeader(Ui& ui, Rect& area, library::Entry& entry) {
    const Theme& t = theme();

    if (bufferFor_ != entry.id) {
        nameBuffer_ = entry.name;
        keyBuffer_ = entry.key;
        char bpm[32];
        std::snprintf(bpm, sizeof(bpm), "%.2f", entry.bpm);
        bpmBuffer_ = entry.bpm > 0.0 ? bpm : std::string();
        bufferFor_ = entry.id;
    }

    Rect nameRow = area.removeFromTop(t.scaled(26.0f));
    if (ui.button(ui.id("library.delete"), nameRow.removeFromRight(t.scaled(56.0f)), "delete",
                  Ui::ButtonStyle::Danger)) {
        library_->remove(entry.id);
        selected_.clear();
        bufferFor_.clear();
        return;
    }
    nameRow.removeFromRight(t.scaled(6.0f));

    if (ui.textField(ui.id("library.name"), nameRow, nameBuffer_, "name")) {
        entry.name = nameBuffer_;
        library_->save(entry.id);
    }

    area.removeFromTop(t.scaled(6.0f));

    // -- tempo and key -----------------------------------------------------
    Rect factsRow = area.removeFromTop(t.scaled(22.0f));
    ui.label(factsRow.removeFromLeft(t.scaled(34.0f)), "bpm", t.textFaint, t.fontSmall);

    if (ui.textField(ui.id("library.bpm"), factsRow.removeFromLeft(t.scaled(70.0f)),
                     bpmBuffer_, "-")) {
        entry.bpm = std::strtod(bpmBuffer_.c_str(), nullptr);
        library_->save(entry.id);
    }

    factsRow.removeFromLeft(t.scaled(12.0f));
    ui.label(factsRow.removeFromLeft(t.scaled(28.0f)), "key", t.textFaint, t.fontSmall);
    if (ui.textField(ui.id("library.key"), factsRow.removeFromLeft(t.scaled(80.0f)),
                     keyBuffer_, "-")) {
        entry.key = keyBuffer_;
        library_->save(entry.id);
    }

    area.removeFromTop(t.scaled(6.0f));

    // -- tags --------------------------------------------------------------
    const library::TagPalette& palette = library_->palette();
    Rect tagRow = area.removeFromTop(t.scaled(22.0f));

    for (int i = 0; i < palette.count(); ++i) {
        const library::Tag& tag = palette.tags()[static_cast<std::size_t>(i)];
        const float width = ui.font(t.fontSmall).textWidth(tag.name) + t.scaled(14.0f);

        if (tagRow.width < width) {
            // Wrapped rather than clipped: the palette is user-extensible, so
            // there is no row width that is guaranteed to be enough.
            if (area.height < t.scaled(26.0f)) break;
            area.removeFromTop(t.scaled(3.0f));
            tagRow = area.removeFromTop(t.scaled(22.0f));
        }

        const Rect chip = tagRow.removeFromLeft(width);
        tagRow.removeFromLeft(t.scaled(3.0f));

        const bool on = entry.hasTag(tag.id);
        bool hovered = false, held = false;
        if (ui.buttonBehaviour(ui.id("library.tag." + tag.id), chip, hovered, held)) {
            if (on) entry.tags.erase(std::remove(entry.tags.begin(), entry.tags.end(), tag.id),
                                     entry.tags.end());
            else entry.tags.push_back(tag.id);
            library_->save(entry.id);
        }

        const Colour colour = tagColour(tag);
        Colour fill = on ? colour.withAlpha(0.5f) : colour.withAlpha(0.12f);
        if (hovered) fill = fill.brightened(1.35f);
        ui.draw().addRectFilled(chip, fill, 2.0f);
        ui.draw().addTextClipped(ui.font(t.fontSmall), chip, on ? t.text : t.textFaint,
                                 tag.name, DrawList::Align::Centre);
    }
}

void LibraryView::drawFiles(Ui& ui, Rect& area, library::Entry& entry) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    ui.separator(area.removeFromTop(t.scaled(9.0f)));
    Rect header = area.removeFromTop(t.scaled(20.0f));
    ui.label(header.removeFromLeft(t.scaled(60.0f)), "audio", t.textDim, t.fontUiBold);
    ui.labelDim(header, "drop files here", DrawList::Align::Right);

    // The whole section takes a drop, so aiming is not required.
    const Rect dropTarget{ area.left(), header.top(), area.width,
                           std::min(area.height + header.height, t.scaled(220.0f)) };
    if (ui.acceptDrop(dropTarget, "file")) {
        library_->addFile(entry.id, ui.dragPayload());
        return;
    }

    area.removeFromTop(t.scaled(3.0f));

    if (entry.files.empty()) {
        ui.labelDim(area.removeFromTop(t.scaled(18.0f)), "no audio referenced yet");
        return;
    }

    // A copy, because opening or removing inside the loop changes the vector.
    const std::vector<std::string> files = entry.files;
    for (const std::string& path : files) {
        if (area.height < t.scaled(24.0f)) break;

        Rect row = area.removeFromTop(t.scaled(20.0f));

        const Rect removeArea = row.removeFromRight(t.scaled(20.0f));
        if (ui.iconButton(ui.id("library.file.remove." + path), removeArea,
                          Ui::Icon::Cross, t.textFaint)) {
            library_->removeFile(entry.id, path);
            return;
        }
        if (ui.isHot(ui.id("library.file.remove." + path)))
            ui.setTooltip("Forget this file. The file itself is not touched.");

        const Rect openArea = row.removeFromRight(t.scaled(22.0f));
        if (ui.iconButton(ui.id("library.file.open." + path), openArea,
                          Ui::Icon::Play, t.accentDim) && onSendToPatch)
            onSendToPatch(path);

        bool hovered = false, held = false;
        ui.buttonBehaviour(ui.id("library.file.row." + path), row, hovered, held);
        if (hovered) {
            list.addRectFilled(row, t.widgetHover, 2.0f);

            // What else points at this file. The question that makes a
            // non-destructive library safe to reorganise, answered on hover
            // rather than behind a command.
            const std::vector<const library::Entry*> users =
                library_->entriesContaining(path);
            std::string tip = path;
            if (users.size() > 1) tip += "\nalso used by " + std::to_string(users.size() - 1)
                                       + " other entr" + (users.size() == 2 ? "y" : "ies");
            ui.setTooltip(tip);
        }

        list.addTextClipped(ui.font(t.fontSmall), row.deflated(t.scaled(3.0f)),
                            t.textDim, pathLeaf(path));
    }
}

void LibraryView::drawMembers(Ui& ui, Rect& area, library::Entry& entry) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    ui.separator(area.removeFromTop(t.scaled(9.0f)));
    Rect header = area.removeFromTop(t.scaled(20.0f));
    ui.label(header.removeFromLeft(t.scaled(100.0f)), "running order", t.textDim, t.fontUiBold);

    // Songs not already in this project, offered as a combo. A song can be in
    // as many projects as it likes, so being here does not remove it anywhere.
    std::vector<std::string> names{ "add a song..." };
    std::vector<std::string> ids{ std::string() };
    for (const library::Entry* song : library_->entriesOfKind(library::EntryKind::Song)) {
        if (std::find(entry.members.begin(), entry.members.end(), song->id)
            != entry.members.end())
            continue;
        names.push_back(song->name);
        ids.push_back(song->id);
    }

    int chosen = 0;
    if (ui.combo(ui.id("library.addmember"), header, names, chosen)
        && chosen > 0 && chosen < static_cast<int>(ids.size())) {
        library_->addMember(entry.id, ids[static_cast<std::size_t>(chosen)]);
        return;
    }

    area.removeFromTop(t.scaled(3.0f));

    if (entry.members.empty()) {
        ui.labelDim(area.removeFromTop(t.scaled(18.0f)), "no songs in this project yet");
        return;
    }

    const std::vector<std::string> members = entry.members;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (area.height < t.scaled(26.0f)) break;

        const std::string& songId = members[i];
        Rect row = area.removeFromTop(t.scaled(22.0f));

        const Rect removeArea = row.removeFromRight(t.scaled(20.0f));
        if (ui.iconButton(ui.id("library.member.remove." + songId), removeArea,
                          Ui::Icon::Cross, t.textFaint)) {
            library_->removeMember(entry.id, songId);
            return;
        }
        if (ui.isHot(ui.id("library.member.remove." + songId)))
            ui.setTooltip("Take it out of this project. The song itself stays.");

        const Rect downArea = row.removeFromRight(t.scaled(20.0f));
        const Rect upArea = row.removeFromRight(t.scaled(20.0f));

        if (i + 1 < members.size()
            && ui.iconButton(ui.id("library.member.down." + songId), downArea,
                             Ui::Icon::Chevron, t.textDim)) {
            library_->moveMember(entry.id, songId, 1);
            return;
        }
        if (i > 0 && ui.iconButton(ui.id("library.member.up." + songId), upArea,
                                   Ui::Icon::ChevronUp, t.textDim)) {
            library_->moveMember(entry.id, songId, -1);
            return;
        }

        char position[16];
        std::snprintf(position, sizeof(position), "%d", static_cast<int>(i) + 1);
        list.addTextClipped(ui.font(t.fontMono), row.removeFromLeft(t.scaled(26.0f)),
                            t.textFaint, position, DrawList::Align::Right);
        row.removeFromLeft(t.scaled(6.0f));

        const library::Entry* song = library_->find(songId);
        bool hovered = false, held = false;
        if (ui.buttonBehaviour(ui.id("library.member.row." + songId), row, hovered, held)
            && song) {
            // Clicking a song in the running order goes to it, which is how
            // anyone expects to get from an album to a track.
            selected_ = songId;
            bufferFor_.clear();
            return;
        }
        if (hovered) list.addRectFilled(row, t.widgetHover, 2.0f);

        list.addTextClipped(ui.font(t.fontSmall), row.deflated(t.scaled(3.0f)),
                            song ? t.textDim : t.danger,
                            song ? song->name : (songId + " (missing)"));
    }
}

void LibraryView::drawLines(Ui& ui, Rect& area, library::Entry& entry, std::string& text,
                            const char* caption, int salt) {
    const Theme& t = theme();

    ui.separator(area.removeFromTop(t.scaled(9.0f)));
    Rect header = area.removeFromTop(t.scaled(20.0f));
    ui.label(header.removeFromLeft(t.scaled(70.0f)), caption, t.textDim, t.fontUiBold);

    std::vector<std::string> lines = splitLines(text);
    const UiId fieldId = ui.id(std::string(caption) + ".edit");

    // A line being typed for the first time is held here rather than written
    // into the entry empty and edited in place. Storing it first does not work:
    // the text is a block joined by newlines, an empty last line leaves no
    // trace in it, and the row being edited would vanish between frames.
    const int newLineIndex = static_cast<int>(lines.size());

    if (ui.iconButton(ui.id(std::string(caption) + ".add"),
                      header.removeFromRight(t.scaled(20.0f)), Ui::Icon::Plus, t.accent)) {
        editingSalt_ = salt;
        editingLine_ = newLineIndex;
        lineBuffer_.clear();
        // Focus has to be given explicitly: a text field only starts editing
        // when it is clicked, and this one is about to appear under no pointer
        // at all.
        ui.beginTextEdit(fieldId, lineBuffer_, false);
    }

    area.removeFromTop(t.scaled(3.0f));

    if (lines.empty() && !(editingSalt_ == salt && editingLine_ == newLineIndex)) {
        ui.labelDim(area.removeFromTop(t.scaled(18.0f)), "empty");
        return;
    }

    const auto commit = [&](std::vector<std::string>& edited) {
        text = joinLines(edited);
        library_->save(entry.id);
        editingLine_ = -1;
    };

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (area.height < t.scaled(22.0f)) break;

        const int index = static_cast<int>(i);
        Rect row = area.removeFromTop(t.scaled(19.0f));

        const bool editing = editingSalt_ == salt && editingLine_ == index;

        const Rect removeArea = row.removeFromRight(t.scaled(18.0f));
        if (ui.iconButton(ui.id(std::string(caption) + ".del." + std::to_string(i)),
                          removeArea, Ui::Icon::Minus, t.textFaint)) {
            lines.erase(lines.begin() + static_cast<long>(i));
            commit(lines);
            return;
        }

        if (editing) {
            if (ui.textField(fieldId, row, lineBuffer_)) {
                lines[i] = lineBuffer_;
                commit(lines);
            }
            continue;
        }

        bool hovered = false, held = false;
        if (ui.buttonBehaviour(ui.id(std::string(caption) + ".line." + std::to_string(i)),
                               row, hovered, held)) {
            editingSalt_ = salt;
            editingLine_ = index;
            lineBuffer_ = lines[i];
            ui.beginTextEdit(fieldId, lineBuffer_, true);
        }
        if (hovered) ui.draw().addRectFilled(row, t.widgetHover, 2.0f);

        ui.draw().addTextClipped(ui.font(t.fontSmall), row.deflated(t.scaled(3.0f)),
                                 lines[i].empty() ? t.textFaint : t.textDim,
                                 lines[i].empty() ? "(blank)" : lines[i]);
    }

    // The line being added, drawn after the ones that exist.
    if (editingSalt_ == salt && editingLine_ == newLineIndex
        && area.height >= t.scaled(22.0f)) {
        Rect row = area.removeFromTop(t.scaled(19.0f));
        row.removeFromRight(t.scaled(18.0f));

        if (ui.textField(fieldId, row, lineBuffer_)) {
            // An empty line committed is a line nobody wanted, so it is dropped
            // rather than stored as a blank row to be tidied up later.
            if (!lineBuffer_.empty()) {
                lines.push_back(lineBuffer_);
                commit(lines);
            } else {
                editingLine_ = -1;
            }
        }
    }
}

void LibraryView::drawDetail(Ui& ui, const Rect& area) {
    const Theme& t = theme();

    library::Entry* entry = selectedEntry();
    if (!entry) {
        ui.labelDim(Rect{ area.left(), area.centre().y - t.scaled(10.0f), area.width,
                          t.scaled(20.0f) },
                    kind_ == library::EntryKind::Project
                        ? "select a project, or make one"
                        : "select a song, or make one",
                    DrawList::Align::Centre);
        return;
    }

    // The whole pane scrolls: a song with forty lines of lyrics and a dozen
    // files is normal, and the alternative is four scroll regions that all end
    // up too short.
    const float contentHeight = t.scaled(1400.0f);
    Rect content = ui.beginScroll(ui.id("library.detail"), area, contentHeight);
    content = content.deflated(t.padding);

    drawHeader(ui, content, *entry);

    // The entry may have been deleted by its own delete button.
    entry = selectedEntry();
    if (!entry) { ui.endScroll(); return; }

    if (kind_ == library::EntryKind::Project) drawMembers(ui, content, *entry);

    drawFiles(ui, content, *entry);

    entry = selectedEntry();
    if (!entry) { ui.endScroll(); return; }

    drawLines(ui, content, *entry, entry->notes, "notes", 0);

    entry = selectedEntry();
    if (entry && kind_ == library::EntryKind::Song)
        drawLines(ui, content, *entry, entry->lyrics, "lyrics", 1);

    ui.endScroll();
}

void LibraryView::render(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();
    if (!library_) return;

    ui.draw().addRectFilled(bounds, t.background);

    if (!library_->isOpen()) {
        ui.labelDim(bounds, "no library folder - check Documents/acomposter/library",
                    DrawList::Align::Centre);
        return;
    }

    Rect area = bounds;
    drawList(ui, area.removeFromLeft(listWidth()));
    drawDetail(ui, area);
}

} // namespace acm::ui
