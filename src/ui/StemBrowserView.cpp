#include "StemBrowserView.h"

#include "../audio/AudioFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace acm::ui {
namespace {

Colour fromArgb(std::uint32_t argb) {
    return Colour{ static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                   static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                   static_cast<float>(argb & 0xFF) / 255.0f,
                   static_cast<float>((argb >> 24) & 0xFF) / 255.0f };
}

bool containsNoCase(const std::string& haystack, const std::string& needle) {
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

} // namespace

void StemBrowserView::initialise(Engine* engine, library::Library* library) {
    engine_ = engine;
    library_ = library;
}

void StemBrowserView::navigateTo(const std::string& directory) {
    if (directory.empty()) return;
    directory_ = directory;
    needsRefresh_ = true;
    selected_ = -1;
}

void StemBrowserView::refresh() {
    files_ = listDirectory(directory_, audiofile::supportedExtensions());
    needsRefresh_ = false;
}

void StemBrowserView::select(int index) {
    if (index < 0 || index >= static_cast<int>(files_.size())) return;
    selected_ = index;

    const DirectoryEntry& entry = files_[static_cast<std::size_t>(index)];
    if (entry.isDirectory) {
        navigateTo(entry.fullPath);
        return;
    }

    if (loadedPath_ == entry.fullPath) return;

    loadError_.clear();
    playhead_ = 0.0;

    audiofile::LoadOptions options;
    options.buildOverview = true;
    options.overviewBuckets = 2048;
    loaded_ = audiofile::load(entry.fullPath, &loadError_, options);
    loadedPath_ = loaded_ ? entry.fullPath : std::string();

    // Selecting a file starts it. Auditioning a folder means hearing each one
    // in turn, and making that two actions instead of one is friction with no
    // purpose.
    if (loaded_ && engine_) engine_->startPreview(loaded_, 0.0);
}

void StemBrowserView::render(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();
    if (needsRefresh_) refresh();

    ui.draw().addRectFilled(bounds, t.background);
    Rect area = bounds.deflated(t.padding);

    // -- header ------------------------------------------------------------
    Rect header = area.removeFromTop(26.0f);
    ui.label(header.removeFromLeft(120.0f), "stem browser", t.text, t.fontTitle);

    if (ui.button(ui.id("stems.up"), header.removeFromRight(40.0f), "up")) {
        const std::string parent = pathParent(directory_);
        if (!parent.empty() && parent != directory_) navigateTo(parent);
    }
    header.removeFromRight(6.0f);

    if (ui.button(ui.id("stems.tags"), header.removeFromRight(90.0f), "edit tags",
                  showTagEditor_ ? Ui::ButtonStyle::Toggle : Ui::ButtonStyle::Normal,
                  showTagEditor_))
        showTagEditor_ = !showTagEditor_;
    header.removeFromRight(6.0f);

    ui.draw().addTextClipped(ui.font(t.fontSmall), header, t.textFaint, directory_,
                             DrawList::Align::Right);

    area.removeFromTop(6.0f);

    // -- tag palette -------------------------------------------------------
    drawTagPalette(ui, area);
    area.removeFromTop(6.0f);

    // -- waveform ----------------------------------------------------------
    Rect waveArea = area.removeFromTop(std::max(120.0f, area.height * 0.34f));
    drawWaveform(ui, waveArea);
    area.removeFromTop(8.0f);

    // -- listing -----------------------------------------------------------
    drawFileList(ui, area);
}

void StemBrowserView::drawTagPalette(Ui& ui, Rect& area) {
    if (!library_) return;

    const Theme& t = theme();
    library::TagPalette& palette = library_->palette();

    // The tag of whatever is selected, so pressing a swatch is visibly a
    // statement about *this* file rather than a mode change.
    const std::string currentTag = (!loadedPath_.empty() && library_)
        ? library_->tagForFile(loadedPath_) : std::string();

    const float rowHeight = 22.0f;
    const float gap = 4.0f;
    Rect row = area.removeFromTop(rowHeight);

    for (int i = 0; i < palette.count(); ++i) {
        const library::Tag& tag = palette.tags()[static_cast<std::size_t>(i)];
        const float width = std::min(ui.font(t.fontSmall).textWidth(tag.name) + 26.0f, area.width);

        if (width > row.width) {
            area.removeFromTop(gap);
            row = area.removeFromTop(rowHeight);
        }

        Rect chip = row.removeFromLeft(width);
        row.removeFromLeft(gap);

        const bool isCurrent = tag.id == currentTag;
        const Colour colour = fromArgb(tag.colour);

        bool hovered = false, held = false;
        if (ui.buttonBehaviour(ui.idFrom(&tag, 1), chip, hovered, held)) {
            if (!loadedPath_.empty()) {
                // Pressing the tag a file already has takes it off, so one
                // control both assigns and clears.
                library_->setTagForFile(loadedPath_, isCurrent ? std::string() : tag.id);
            }
        }

        Colour fill = isCurrent ? colour.withAlpha(0.55f) : colour.withAlpha(0.16f);
        if (hovered) fill = fill.brightened(1.35f);
        ui.draw().addRectFilled(chip, fill, t.cornerRadius);
        ui.draw().addRect(chip, colour.withAlpha(isCurrent ? 1.0f : 0.5f), 1.0f, t.cornerRadius);
        ui.draw().addTextClipped(ui.font(t.fontSmall), chip.deflated(4.0f),
                                 isCurrent ? t.text : t.textDim, tag.name,
                                 DrawList::Align::Centre);

        if (hovered) {
            ui.setTooltip(loadedPath_.empty()
                ? "Select a file first"
                : (isCurrent ? "Click to clear this tag" : "Tag the selected file as " + tag.name));
        }
    }

    // -- editor ------------------------------------------------------------
    if (!showTagEditor_) return;

    area.removeFromTop(6.0f);
    Rect editorRow = area.removeFromTop(22.0f);

    if (ui.button(ui.id("stems.tag.add"), editorRow.removeFromLeft(70.0f), "new tag")) {
        palette.add("new tag", 0xFF8A8F98u);
        library_->savePalette();
        editingTag_ = palette.count() - 1;
        tagNameBuffer_ = "new tag";
    }
    editorRow.removeFromLeft(8.0f);

    if (editingTag_ >= 0 && editingTag_ < palette.count()) {
        if (ui.textField(ui.id("stems.tag.name"), editorRow.removeFromLeft(160.0f), tagNameBuffer_)) {
            palette.rename(editingTag_, tagNameBuffer_);
            library_->savePalette();
        }
        editorRow.removeFromLeft(8.0f);

        if (ui.button(ui.id("stems.tag.del"), editorRow.removeFromLeft(60.0f), "delete",
                      Ui::ButtonStyle::Danger)) {
            palette.remove(editingTag_);
            library_->savePalette();
            editingTag_ = -1;
        }
        editorRow.removeFromLeft(8.0f);
    }

    ui.draw().addTextClipped(ui.font(t.fontSmall), editorRow, t.textFaint,
                             "shift-click a tag to rename or recolour it");

    // Shift-click picks a tag up for editing without assigning it.
    if (ui.input().shift) {
        for (int i = 0; i < palette.count(); ++i) {
            if (ui.isHot(ui.idFrom(&palette.tags()[static_cast<std::size_t>(i)], 1))
                && ui.input().mousePressed[static_cast<int>(MouseButton::Left)]) {
                editingTag_ = i;
                tagNameBuffer_ = palette.tags()[static_cast<std::size_t>(i)].name;
            }
        }
    }
}

void StemBrowserView::drawWaveform(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(bounds, t.panelSunken, t.cornerRadius);
    list.addRect(bounds, t.border, 1.0f, t.cornerRadius);

    if (!loaded_) {
        list.addTextClipped(ui.font(t.fontUi), bounds,
                            loadError_.empty() ? t.textFaint : t.danger,
                            loadError_.empty() ? "select a file to audition it" : loadError_,
                            DrawList::Align::Centre);
        return;
    }

    Rect plot = bounds.deflated(4.0f);

    // The tag's colour, so the waveform itself says what the file has been
    // called - the one place the tag is unmissable while listening.
    Colour trace = t.accent;
    if (library_) {
        if (const library::Tag* tag = library_->palette().find(library_->tagForFile(loadedPath_)))
            trace = fromArgb(tag->colour);
    }

    const auto& overview = loaded_->overview();
    if (!overview.minimum.empty()) {
        const float centreY = plot.centre().y;
        const float halfHeight = plot.height * 0.46f;
        const int columns = static_cast<int>(plot.width);

        for (int x = 0; x < columns; ++x) {
            const std::size_t index = static_cast<std::size_t>(
                static_cast<float>(x) / static_cast<float>(std::max(1, columns))
                * static_cast<float>(overview.minimum.size()));
            if (index >= overview.minimum.size()) break;

            const float low = overview.minimum[index];
            const float high = overview.maximum[index];

            list.addRectFilled(Rect{ plot.left() + static_cast<float>(x),
                                     centreY - high * halfHeight,
                                     1.0f,
                                     std::max(1.0f, (high - low) * halfHeight) },
                               trace.withAlpha(0.85f));
        }
    }

    // -- playhead ----------------------------------------------------------
    const double duration = loaded_->durationSeconds();
    const double enginePosition = engine_ ? engine_->previewPositionSeconds() : -1.0;
    const bool playing = engine_ && engine_->previewPlaying() && enginePosition >= 0.0;
    if (playing) playhead_ = enginePosition;

    bool hovered = false, held = false;
    const UiId scrubId = ui.id("stems.scrub");
    ui.buttonBehaviour(scrubId, bounds, hovered, held);

    if (ui.isActive(scrubId) && duration > 0.0) {
        const float fraction = clampValue((ui.input().mousePosition.x - plot.left())
                                              / std::max(1.0f, plot.width), 0.0f, 1.0f);
        playhead_ = static_cast<double>(fraction) * duration;
        // Scrubbing seeks the audition rather than only moving a marker, which
        // is the whole reason the playhead is movable.
        if (engine_) {
            if (!engine_->previewPlaying()) engine_->startPreview(loaded_, playhead_);
            else engine_->seekPreview(playhead_);
        }
    }
    if (hovered) ui.setCursor(Cursor::ResizeHorizontal);

    if (duration > 0.0) {
        const float x = plot.left() + plot.width
                      * clampValue(static_cast<float>(playhead_ / duration), 0.0f, 1.0f);
        list.addRectFilled(Rect{ x - 1.0f, plot.top(), 2.0f, plot.height },
                           playing ? t.text : t.textDim);
    }

    // -- transport ---------------------------------------------------------
    Rect controls = Rect{ bounds.left() + 6.0f, bounds.bottom() - 26.0f, 200.0f, 20.0f };

    if (ui.button(ui.id("stems.play"), controls.removeFromLeft(52.0f),
                  playing ? "stop" : "play",
                  playing ? Ui::ButtonStyle::Danger : Ui::ButtonStyle::Primary)) {
        if (playing) engine_->stopPreview();
        else if (engine_) engine_->startPreview(loaded_, playhead_);
    }
    controls.removeFromLeft(6.0f);

    if (ui.button(ui.id("stems.topatch"), controls.removeFromLeft(88.0f), "to patch")
        && onSendToPatch && !loadedPath_.empty()) {
        onSendToPatch(loadedPath_, library_ ? library_->tagForFile(loadedPath_) : std::string());
    }

    char readout[96];
    std::snprintf(readout, sizeof(readout), "%.2f / %.2f s   %d ch   %.0f Hz",
                  playhead_, duration, loaded_->channels(), loaded_->sampleRate());
    list.addTextClipped(ui.font(t.fontSmall),
                        Rect{ bounds.right() - 220.0f, bounds.bottom() - 24.0f, 214.0f, 16.0f },
                        t.textFaint, readout, DrawList::Align::Right);
}

void StemBrowserView::drawFileList(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();

    Rect area = bounds;
    Rect filterRow = area.removeFromTop(22.0f);
    ui.textField(ui.id("stems.filter"), filterRow.removeFromLeft(260.0f), filter_, "filter");
    area.removeFromTop(6.0f);

    std::vector<int> visible;
    for (int i = 0; i < static_cast<int>(files_.size()); ++i)
        if (containsNoCase(files_[static_cast<std::size_t>(i)].name, filter_)) visible.push_back(i);

    const float rowHeight = 22.0f;
    Rect content = ui.beginScroll(ui.id("stems.list"), area,
                                  static_cast<float>(visible.size()) * rowHeight);

    if (visible.empty()) {
        ui.draw().addTextClipped(ui.font(t.fontSmall), content.removeFromTop(40.0f), t.textFaint,
                                 files_.empty() ? "no audio here" : "nothing matches that filter",
                                 DrawList::Align::Centre);
    }

    for (int index : visible) {
        const DirectoryEntry& entry = files_[static_cast<std::size_t>(index)];
        const Rect row = content.removeFromTop(rowHeight);
        if (row.bottom() < area.top() - rowHeight || row.top() > area.bottom() + rowHeight) continue;

        bool hovered = false, held = false;
        const bool clicked = ui.buttonBehaviour(ui.idFrom(&entry, 1), row, hovered, held);

        const bool isSelected = index == selected_;
        if (isSelected) ui.draw().addRectFilled(row, t.accent.withAlpha(0.12f), t.cornerRadius);
        else if (hovered) ui.draw().addRectFilled(row, t.widgetHover, t.cornerRadius);

        Rect rowContent = row.deflated(4.0f);

        // The tag swatch leads the row, so an untagged file is obvious at a
        // glance and a folder of them reads as work still to do.
        const Rect swatch = rowContent.removeFromLeft(10.0f);
        rowContent.removeFromLeft(6.0f);

        if (!entry.isDirectory && library_) {
            const std::string tagId = library_->tagForFile(entry.fullPath);
            if (const library::Tag* tag = library_->palette().find(tagId))
                ui.draw().addRectFilled(swatch.deflated(1.0f), fromArgb(tag->colour), 2.0f);
            else
                ui.draw().addRect(swatch.deflated(1.0f), t.border, 1.0f, 2.0f);
        }

        ui.drawIcon(ui.draw(), rowContent.removeFromLeft(14.0f),
                    entry.isDirectory ? Ui::Icon::Folder : Ui::Icon::Wave,
                    entry.isDirectory ? t.control : t.textDim);
        rowContent.removeFromLeft(4.0f);

        ui.draw().addTextClipped(ui.font(t.fontUi), rowContent,
                                 isSelected ? t.text : t.textDim, entry.name);

        if (clicked) select(index);
    }

    ui.endScroll();
}

} // namespace acm::ui
