#include "LibrarianView.h"

#include "../audio/AudioFile.h"
#include "../core/AppPaths.h"
#include <functional>
#include "../core/FileIo.h"
#include "../dsp/Fft.h"
#include "../platform/DragOut.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace acm::ui {
namespace {

float detailWidth() { return theme().scaled(320.0f); }
float rowHeight() { return theme().scaled(20.0f); }

// Files longer than this do not get the 3D view. A spectrogram of a two-minute
// loop drawn as a landscape is a wall, and the point of the thing is to see the
// shape of a single hit.
constexpr double kSpectralMaxSeconds = 5.0;

Colour tagColour(const library::Tag& tag) {
    return Colour{ static_cast<float>((tag.colour >> 16) & 0xFF) / 255.0f,
                   static_cast<float>((tag.colour >> 8) & 0xFF) / 255.0f,
                   static_cast<float>(tag.colour & 0xFF) / 255.0f, 1.0f };
}

// Cold at the bottom of the spectrum, hot at the top. The same reading as any
// spectrogram anyone has seen before, which is worth more than a prettier one.
Colour heatColour(float value, const Theme& t) {
    const float v = clampValue(value, 0.0f, 1.0f);
    if (v < 0.5f) return gfx::lerp(t.panelSunken, t.accentDim, v * 2.0f);
    return gfx::lerp(t.accent, t.warning, (v - 0.5f) * 2.0f);
}

const char* kKeyNames[12] = { "A", "A#", "B", "C", "C#", "D",
                              "D#", "E", "F", "F#", "G", "G#" };

} // namespace

void LibrarianView::initialise(Engine* engine, library::Library* library) {
    engine_ = engine;
    library_ = library;
    wizard_.initialise(engine, library);
    wizard_.onFolderChanged = [this] { if (!folder_.empty()) openFolder(folder_); };
}

void LibrarianView::serviceFromMessageThread() {
    index_.serviceFromMessageThread();
}

void LibrarianView::openFolder(std::string utf8Path) {
    if (utf8Path.empty()) return;

    folder_ = std::move(utf8Path);
    selected_.clear();
    similarTo_.clear();
    loaded_.reset();
    spectrogram_ = library::Spectrogram{};

    // Keyed by folder, so switching between two sample libraries does not make
    // each one re-analyse the other's work. Hashed rather than derived from the
    // path, because a path is not a file name.
    const std::string cache = pathJoin(paths::applicationData(),
                                       "index-"
                                           + std::to_string(std::hash<std::string>{}(folder_))
                                           + ".json");
    index_.loadCache(cache);
    index_.scan(folder_, cache);
}

void LibrarianView::select(const std::string& path) {
    selected_ = path;
    loaded_.reset();
    spectrogram_ = library::Spectrogram{};

    if (path.empty()) return;

    std::string error;
    loaded_ = audiofile::load(path, &error);
    if (!loaded_) return;

    if (loaded_->durationSeconds() <= kSpectralMaxSeconds)
        spectrogram_ = library::computeSpectrogram(*loaded_);

    if (engine_) engine_->startPreview(loaded_, 0.0);
}

// ---------------------------------------------------------------------------
// Toolbar and filters
// ---------------------------------------------------------------------------

void LibrarianView::drawToolbar(Ui& ui, Rect& area) {
    const Theme& t = theme();
    Rect bar = area.removeFromTop(t.scaled(32.0f));

    ui.draw().addRectFilled(bar, t.panelHeader);
    ui.draw().addRectFilled(Rect{ bar.left(), bar.bottom() - 1.0f, bar.width, 1.0f }, t.border);

    Rect row = bar.deflated(t.smallPadding);

    if (ui.button(ui.id("librarian.browse"), row.removeFromLeft(t.scaled(64.0f)), "folder")
        && onBrowseForFolder) {
        const std::string chosen = onBrowseForFolder();
        if (!chosen.empty()) openFolder(chosen);
    }
    row.removeFromLeft(t.scaled(8.0f));

    const library::ScanProgress progress = index_.progress();

    if (progress.running) {
        if (ui.button(ui.id("librarian.cancel"), row.removeFromRight(t.scaled(60.0f)), "stop",
                      Ui::ButtonStyle::Danger))
            index_.cancelScan();

        char status[192];
        std::snprintf(status, sizeof(status), "%d of %d  (%d reused)  %s",
                      progress.analysed + progress.reused, progress.found, progress.reused,
                      progress.currentFile.c_str());
        ui.labelDim(row.removeFromRight(row.width * 0.6f), status, DrawList::Align::Right);
    } else if (!folder_.empty()) {
        if (ui.button(ui.id("librarian.rescan"), row.removeFromRight(t.scaled(64.0f)), "rescan"))
            openFolder(folder_);
        row.removeFromRight(t.scaled(6.0f));

        // The rebuilt folder is a sibling of the source rather than inside it,
        // so a rebuild cannot end up scanning its own output on the next pass.
        if (!index_.files().empty()
            && ui.button(ui.id("librarian.wizard"), row.removeFromRight(t.scaled(70.0f)),
                         "rebuild", Ui::ButtonStyle::Primary)) {
            wizard_.begin(&index_, pathJoin(pathParent(folder_),
                                            pathLeaf(folder_) + " rebuilt"));
        }
        if (ui.isHot(ui.id("librarian.wizard")))
            ui.setTooltip("Work through the folder file by file, into a new one beside it");

        char status[64];
        std::snprintf(status, sizeof(status), "%d files",
                      static_cast<int>(index_.files().size()));
        ui.labelDim(row.removeFromRight(t.scaled(80.0f)), status, DrawList::Align::Right);
    }

    ui.draw().addTextClipped(ui.font(t.fontSmall), row, t.textDim,
                             folder_.empty() ? "no folder chosen" : folder_);
}

void LibrarianView::drawFilters(Ui& ui, Rect& area) {
    const Theme& t = theme();
    Rect bar = area.removeFromTop(t.scaled(30.0f));
    ui.draw().addRectFilled(bar, t.panel);

    Rect row = bar.deflated(t.scaled(4.0f));

    ui.textField(ui.id("librarian.filter"), row.removeFromLeft(t.scaled(160.0f)),
                 filterBuffer_, "search");
    filter_.text = filterBuffer_;
    row.removeFromLeft(t.scaled(8.0f));

    // -- key ---------------------------------------------------------------
    ui.label(row.removeFromLeft(t.scaled(24.0f)), "key", t.textFaint, t.fontSmall);

    std::vector<std::string> keys{ "any" };
    for (const char* name : kKeyNames) keys.push_back(name);

    int keyIndex = keyFilter_ == -128 ? 0 : ((keyFilter_ % 12) + 12) % 12 + 1;
    if (ui.combo(ui.id("librarian.key"), row.removeFromLeft(t.scaled(70.0f)), keys, keyIndex))
        keyFilter_ = keyIndex == 0 ? -128 : keyIndex - 1;
    filter_.semitonesFromA4 = keyFilter_;

    row.removeFromLeft(t.scaled(8.0f));

    bool pitched = filter_.pitchedOnly;
    if (ui.checkbox(ui.id("librarian.pitched"), row.removeFromLeft(t.scaled(80.0f)),
                    "pitched", pitched))
        filter_.pitchedOnly = pitched;
    if (ui.isHot(ui.id("librarian.pitched")))
        ui.setTooltip("Only files with a pitch confident enough to name");

    // -- tag ---------------------------------------------------------------
    if (library_) {
        std::vector<std::string> tagNames{ "all tags" };
        std::vector<std::string> tagIds{ std::string() };
        for (const library::Tag& tag : library_->palette().tags()) {
            tagNames.push_back(tag.name);
            tagIds.push_back(tag.id);
        }

        int tagIndex = 0;
        for (std::size_t i = 0; i < tagIds.size(); ++i)
            if (tagIds[i] == filter_.tagId) tagIndex = static_cast<int>(i);

        if (ui.combo(ui.id("librarian.tag"), row.removeFromLeft(t.scaled(110.0f)),
                     tagNames, tagIndex))
            filter_.tagId = tagIds[static_cast<std::size_t>(tagIndex)];
    }

    // -- sort --------------------------------------------------------------
    Rect right = row;

    if (ui.button(ui.id("librarian.direction"), right.removeFromRight(t.scaled(26.0f)),
                  sortDescending_ ? "v" : "^"))
        sortDescending_ = !sortDescending_;
    right.removeFromRight(t.scaled(4.0f));

    std::vector<std::string> sorts;
    for (int i = 0; i < static_cast<int>(library::SortKey::Count); ++i)
        sorts.push_back(library::toString(static_cast<library::SortKey>(i)));

    int sortIndex = static_cast<int>(sortKey_);
    if (ui.combo(ui.id("librarian.sort"), right.removeFromRight(t.scaled(90.0f)),
                 sorts, sortIndex))
        sortKey_ = static_cast<library::SortKey>(sortIndex);

    ui.label(right.removeFromRight(t.scaled(34.0f)), "sort", t.textFaint, t.fontSmall);
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

void LibrarianView::drawList(Ui& ui, const Rect& area) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    if (folder_.empty()) {
        ui.labelDim(area, "choose a folder of samples to index", DrawList::Align::Centre);
        return;
    }

    // One drag per press: rearmed only once the button is genuinely up.
    if (!ui.input().mouseDown[static_cast<int>(MouseButton::Left)]) draggingOut_ = false;

    const std::vector<const library::IndexedFile*> rows =
        index_.query(filter_, sortKey_, sortDescending_,
                     sortKey_ == library::SortKey::Similarity ? similarTo_ : std::string());

    if (rows.empty()) {
        ui.labelDim(area, index_.scanning() ? "scanning..." : "nothing matches",
                    DrawList::Align::Centre);
        return;
    }

    Rect content = ui.beginScroll(ui.id("librarian.list"), area,
                                  static_cast<float>(rows.size()) * rowHeight());

    const library::IndexedFile* reference =
        similarTo_.empty() ? nullptr : index_.find(similarTo_);

    for (const library::IndexedFile* file : rows) {
        Rect row = content.removeFromTop(rowHeight());
        if (row.bottom() < area.top() || row.top() > area.bottom()) continue;

        const bool isSelected = file->path == selected_;
        bool hovered = false, held = false;
        if (ui.buttonBehaviour(ui.id("librarian.row." + file->path), row, hovered, held))
            select(file->path);

        // Held and moved: hand the file to Windows, so it can be dropped into a
        // DAW, an editor or a folder. The threshold is what separates the
        // gesture from a click that wobbled, and it is in design units because
        // six device pixels is a different gesture on every display.
        if (held && !draggingOut_
            && (ui.input().mousePosition - ui.dragStart()).length() > t.scaled(6.0f)) {
            draggingOut_ = true;
            // The call blocks for the length of the drag, and the button-up
            // that ends it goes to OLE rather than to us - so the row is
            // released here rather than waiting for an event that will not
            // arrive.
            platform::dragOutFiles({ file->path });
            ui.clearActive();
        }

        if (isSelected) list.addRectFilled(row, t.selection.withAlpha(0.20f), 2.0f);
        else if (hovered) list.addRectFilled(row, t.widgetHover, 2.0f);

        if (file->path == similarTo_)
            list.addRectFilled(Rect{ row.left(), row.top(), t.scaled(2.0f), row.height },
                               t.warning);

        Rect cells = row.deflated(t.scaled(3.0f));

        // Right to left, so the name gets whatever is left rather than the
        // columns being squeezed by a long file name.
        if (sortKey_ == library::SortKey::Similarity && reference) {
            char score[16];
            std::snprintf(score, sizeof(score), "%.0f%%",
                          static_cast<double>(library::similarity(file->analysis,
                                                                  reference->analysis)) * 100.0);
            list.addTextClipped(ui.font(t.fontMono), cells.removeFromRight(t.scaled(44.0f)),
                                t.warning, score, DrawList::Align::Right);
        }

        char level[16];
        std::snprintf(level, sizeof(level), "%.0f", static_cast<double>(file->analysis.centroidHz));
        list.addTextClipped(ui.font(t.fontMono), cells.removeFromRight(t.scaled(56.0f)),
                            t.textFaint, level, DrawList::Align::Right);

        list.addTextClipped(ui.font(t.fontMono), cells.removeFromRight(t.scaled(44.0f)),
                            file->analysis.noteName.empty() ? t.textFaint : t.accentDim,
                            file->analysis.noteName.empty() ? "-" : file->analysis.noteName,
                            DrawList::Align::Right);

        char length[24];
        std::snprintf(length, sizeof(length), "%.2fs", file->analysis.durationSeconds);
        list.addTextClipped(ui.font(t.fontMono), cells.removeFromRight(t.scaled(56.0f)),
                            t.textFaint, length, DrawList::Align::Right);

        // The tag swatch leads the row, as it does everywhere else.
        const Rect swatch = cells.removeFromLeft(t.scaled(10.0f));
        cells.removeFromLeft(t.scaled(4.0f));
        if (library_ && !file->tagId.empty()) {
            if (const library::Tag* tag = library_->palette().find(file->tagId))
                list.addRectFilled(swatch.deflated(t.scaled(2.0f)), tagColour(*tag), 2.0f);
        }

        list.addTextClipped(ui.font(t.fontSmall), cells,
                            isSelected ? t.text : t.textDim, file->name);
    }

    ui.endScroll();
}

// ---------------------------------------------------------------------------
// The 3D spectral view
// ---------------------------------------------------------------------------

void LibrarianView::drawSpectral3D(Ui& ui, const Rect& area) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(area, t.panelSunken, t.cornerRadius);
    list.addRect(area, t.border, 1.0f, t.cornerRadius);

    if (spectrogram_.frames <= 1 || spectrogram_.bins <= 1) {
        list.addTextClipped(ui.font(t.fontSmall), area, t.textFaint,
                            loaded_ && loaded_->durationSeconds() > kSpectralMaxSeconds
                                ? "too long for the spectral view"
                                : "select a short file",
                            DrawList::Align::Centre);
        return;
    }

    // Dragging spins and tilts it. A fixed projection reads as a texture; being
    // able to turn it is what makes it legible as a shape.
    const UiId id = ui.id("librarian.spectral");
    bool hovered = false, held = false;
    ui.buttonBehaviour(id, area, hovered, held);
    if (ui.isActive(id)) {
        const Vec2 delta = ui.input().mouseDelta;
        spinAngle_ = clampValue(spinAngle_ + delta.x * 0.006f, 0.15f, 1.40f);
        tiltAngle_ = clampValue(tiltAngle_ - delta.y * 0.005f, 0.08f, 1.10f);
        ui.setCursor(Cursor::Crosshair);
    }

    // A plain axonometric projection: no perspective, no depth buffer, just two
    // axes laid down at an angle and a height. Drawn back to front, so the near
    // rows cover the far ones without any sorting beyond the loop order.
    constexpr int kRowCount = 48;
    const int columnStep = std::max(1, spectrogram_.frames / 120);

    const float cosSpin = std::cos(spinAngle_);
    const float sinSpin = std::sin(spinAngle_);
    const float depth = std::sin(tiltAngle_);

    const Rect plot = area.deflated(t.scaled(8.0f));
    const float height = plot.height * 0.42f;

    // Only the bottom two thirds of the spectrum: everything above ~10 kHz on
    // most material is a flat floor that adds rows and says nothing.
    const int usedBins = std::max(2, spectrogram_.bins * 2 / 3);

    const float spanX = plot.width * 0.72f;
    const float spanZ = plot.width * 0.26f;

    const auto project = [&](float u, float v, float magnitude) {
        // u runs along time, v along frequency, both 0..1.
        const float x = plot.left() + plot.width * 0.16f + u * spanX * cosSpin + v * spanZ * sinSpin;
        const float y = plot.bottom() - t.scaled(6.0f)
                      - (u * spanX * sinSpin + v * spanZ * cosSpin) * depth
                      - magnitude * height;
        return Vec2{ x, y };
    };

    std::vector<Vec2> line;
    line.reserve(static_cast<std::size_t>(spectrogram_.frames / columnStep) + 2);

    // Rows spaced logarithmically in frequency, not linearly. A kick's entire
    // spectrum lives in the first four of 170 linear bins, so a linear stack of
    // rows is one spike and a hundred and sixty flat lines - a picture of the
    // FFT's resolution rather than of the sound.
    for (int row = kRowCount - 1; row >= 0; --row) {
        const float v = static_cast<float>(row) / static_cast<float>(kRowCount - 1);
        const int bin = std::clamp(
            static_cast<int>(std::round(std::pow(static_cast<float>(usedBins), v))),
            1, usedBins - 1);

        line.clear();
        float rowPeak = 0.0f;

        for (int frame = 0; frame < spectrogram_.frames; frame += columnStep) {
            const float u = static_cast<float>(frame)
                          / static_cast<float>(spectrogram_.frames - 1);
            const float magnitude = spectrogram_.at(frame, bin);
            rowPeak = std::max(rowPeak, magnitude);
            line.push_back(project(u, v, magnitude));
        }

        if (line.size() < 2) continue;

        // Fainter toward the back, so the shape has depth without any shading.
        const float fade = 0.35f + 0.65f * (1.0f - v);
        list.addPolyline(line.data(), static_cast<int>(line.size()),
                         heatColour(rowPeak, t).withAlpha(fade), false, 1.2f);
    }

    // The ground plane, drawn as the whole floor rather than two rays. Dense
    // material never reaches the noise floor, so the surface floats - and above
    // two lines going off into nothing that reads as a drawing error, while
    // above a rectangle it reads as height, which is what it is.
    const Vec2 origin = project(0.0f, 0.0f, 0.0f);
    const Vec2 timeEnd = project(1.0f, 0.0f, 0.0f);
    const Vec2 freqEnd = project(0.0f, 1.0f, 0.0f);
    const Vec2 farCorner = project(1.0f, 1.0f, 0.0f);

    const Vec2 floorOutline[5] = { origin, timeEnd, farCorner, freqEnd, origin };
    list.addPolyline(floorOutline, 5, t.border, false, 1.0f);

    char timeLabel[32];
    std::snprintf(timeLabel, sizeof(timeLabel), "%.2fs",
                  loaded_ ? loaded_->durationSeconds() : 0.0);
    list.addTextClipped(ui.font(t.fontSmall),
                        Rect{ timeEnd.x - t.scaled(40.0f), timeEnd.y + t.scaled(2.0f),
                              t.scaled(44.0f), t.scaled(12.0f) },
                        t.textFaint, timeLabel, DrawList::Align::Right);

    const double topHz = spectrogram_.sampleRate * 0.5
                       * static_cast<double>(usedBins) / static_cast<double>(spectrogram_.bins);
    char freqLabel[32];
    std::snprintf(freqLabel, sizeof(freqLabel), "%.0fk", topHz / 1000.0);
    // Above and to the left of the far edge, so it does not sit on the floor
    // outline it is labelling.
    list.addTextClipped(ui.font(t.fontSmall),
                        Rect{ freqEnd.x - t.scaled(36.0f), freqEnd.y - t.scaled(14.0f),
                              t.scaled(34.0f), t.scaled(12.0f) },
                        t.textFaint, freqLabel, DrawList::Align::Right);

    if (hovered) ui.setTooltip("Drag to turn it");
}

void LibrarianView::drawBands(Ui& ui, const Rect& area, const library::Analysis& analysis) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(area, t.panelSunken, 2.0f);

    float loudest = 0.0f;
    for (const float band : analysis.bands) loudest = std::max(loudest, band);
    if (loudest <= 0.0f) return;

    const float barWidth = area.width / static_cast<float>(library::kSpectrumBands);
    for (int i = 0; i < library::kSpectrumBands; ++i) {
        const float amount = analysis.bands[i] / loudest;
        const float barHeight = area.height * amount;

        const Rect bar{ area.left() + barWidth * static_cast<float>(i) + t.scaled(1.0f),
                        area.bottom() - barHeight,
                        barWidth - t.scaled(2.0f), barHeight };
        list.addRectFilled(bar, heatColour(static_cast<float>(i)
                                           / static_cast<float>(library::kSpectrumBands - 1), t));
    }
}

// ---------------------------------------------------------------------------
// The detail pane
// ---------------------------------------------------------------------------

std::string LibrarianView::writeProcessedCopy(const std::string& suffix, bool normalise,
                                              bool trim) {
    if (!loaded_ || loaded_->empty() || selected_.empty()) return {};

    std::int64_t first = 0;
    std::int64_t last = loaded_->frames() - 1;

    if (trim) {
        // A tenth of a percent of full scale: below that is either silence or
        // the noise floor of whatever recorded it.
        constexpr float kThreshold = 0.001f;

        const auto sounding = [&](std::int64_t frame) {
            for (int c = 0; c < loaded_->channels(); ++c)
                if (std::fabs(loaded_->channel(c)[frame]) > kThreshold) return true;
            return false;
        };

        while (first < last && !sounding(first)) ++first;
        while (last > first && !sounding(last)) --last;
    }

    const std::int64_t frames = last - first + 1;
    if (frames <= 0) return {};

    SampleBuffer out(loaded_->channels(), frames, loaded_->sampleRate());

    float peak = 0.0f;
    for (int c = 0; c < loaded_->channels(); ++c)
        for (std::int64_t i = 0; i < frames; ++i)
            peak = std::max(peak, std::fabs(loaded_->channel(c)[first + i]));

    // Just under full scale. Exactly 1.0 clips on any resampler downstream.
    const float gain = (normalise && peak > 1.0e-6f) ? (0.98f / peak) : 1.0f;

    for (int c = 0; c < loaded_->channels(); ++c) {
        const float* source = loaded_->channel(c);
        float* destination = out.channelForWrite(c);
        for (std::int64_t i = 0; i < frames; ++i) destination[i] = source[first + i] * gain;
    }

    // Written beside the original with a suffix. Never over it: a librarian
    // that can damage the library is not one anybody will point at a samples
    // folder, and undo is not a thing a file system offers.
    const std::string directory = pathParent(selected_);
    const std::string stem = pathStem(pathLeaf(selected_));
    std::string target = pathJoin(directory, stem + suffix + ".wav");

    for (int attempt = 2; fileExists(target) && attempt < 100; ++attempt)
        target = pathJoin(directory, stem + suffix + "-" + std::to_string(attempt) + ".wav");

    if (!audiofile::writeWav(target, out)) return {};
    return target;
}

void LibrarianView::drawDetail(Ui& ui, const Rect& area) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(area, t.panel);
    list.addRectFilled(Rect{ area.left(), area.top(), 1.0f, area.height }, t.border);

    Rect column = area.deflated(t.smallPadding);

    const library::IndexedFile* file = selected_.empty() ? nullptr : index_.find(selected_);
    if (!file) {
        ui.labelDim(column, "select a file", DrawList::Align::Centre);
        return;
    }

    ui.label(column.removeFromTop(t.scaled(20.0f)), file->name, t.text, t.fontUiBold);
    column.removeFromTop(t.scaled(4.0f));

    // -- the 3D view -------------------------------------------------------
    drawSpectral3D(ui, column.removeFromTop(t.scaled(180.0f)));
    column.removeFromTop(t.scaled(6.0f));

    drawBands(ui, column.removeFromTop(t.scaled(34.0f)), file->analysis);
    column.removeFromTop(t.scaled(6.0f));

    // -- facts -------------------------------------------------------------
    const auto fact = [&](const char* caption, const std::string& value) {
        if (column.height < t.scaled(18.0f)) return;
        Rect row = column.removeFromTop(t.scaled(16.0f));
        ui.label(row.removeFromLeft(t.scaled(74.0f)), caption, t.textFaint, t.fontSmall);
        list.addTextClipped(ui.font(t.fontMono), row, t.textDim, value);
    };

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f s", file->analysis.durationSeconds);
    fact("length", buffer);

    std::snprintf(buffer, sizeof(buffer), "%.0f Hz  %d ch",
                  file->analysis.sampleRate, file->analysis.channels);
    fact("format", buffer);

    if (!file->analysis.noteName.empty()) {
        std::snprintf(buffer, sizeof(buffer), "%s  (%.1f Hz, %.0f%%)",
                      file->analysis.noteName.c_str(), file->analysis.pitchHz,
                      static_cast<double>(file->analysis.pitchConfidence) * 100.0);
        fact("pitch", buffer);
    } else {
        fact("pitch", "unpitched");
    }

    std::snprintf(buffer, sizeof(buffer), "%.0f Hz", file->analysis.centroidHz);
    fact("bright", buffer);

    std::snprintf(buffer, sizeof(buffer), "%.1f dB peak", 20.0 * std::log10(
        std::max(static_cast<double>(file->analysis.peak), 1.0e-6)));
    fact("level", buffer);

    if (file->analysis.bpm > 0.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f", file->analysis.bpm);
        fact("tempo", buffer);
    }

    if (!file->analysis.filenameNumbers.empty()) {
        std::string numbers;
        for (const int number : file->analysis.filenameNumbers) {
            if (!numbers.empty()) numbers += ", ";
            numbers += std::to_string(number);
        }
        fact("numbers", numbers);
    }

    column.removeFromTop(t.scaled(6.0f));

    // -- tags --------------------------------------------------------------
    if (library_ && column.height > t.scaled(30.0f)) {
        Rect tagRow = column.removeFromTop(t.scaled(20.0f));

        for (const library::Tag& tag : library_->palette().tags()) {
            const float width = ui.font(t.fontSmall).textWidth(tag.name) + t.scaled(12.0f);
            if (tagRow.width < width) {
                if (column.height < t.scaled(24.0f)) break;
                column.removeFromTop(t.scaled(2.0f));
                tagRow = column.removeFromTop(t.scaled(20.0f));
            }

            const Rect chip = tagRow.removeFromLeft(width);
            tagRow.removeFromLeft(t.scaled(2.0f));

            const bool on = file->tagId == tag.id;
            bool hovered = false, held = false;
            if (ui.buttonBehaviour(ui.id("librarian.tagchip." + tag.id), chip, hovered, held)) {
                const std::string next = on ? std::string() : tag.id;
                // The library is the store of record; the index is kept in step
                // so the list recolours without a rescan.
                library_->setTagForFile(selected_, next);
                index_.setTag(selected_, next);
            }

            const Colour colour = tagColour(tag);
            Colour fill = on ? colour.withAlpha(0.5f) : colour.withAlpha(0.12f);
            if (hovered) fill = fill.brightened(1.35f);
            list.addRectFilled(chip, fill, 2.0f);
            list.addTextClipped(ui.font(t.fontSmall), chip, on ? t.text : t.textFaint,
                                tag.name, DrawList::Align::Centre);
        }
    }

    column.removeFromTop(t.scaled(8.0f));

    // -- actions -----------------------------------------------------------
    Rect actions = column.removeFromTop(t.scaled(22.0f));

    if (ui.button(ui.id("librarian.similar"), actions.removeFromLeft(t.scaled(88.0f)),
                  "find similar", Ui::ButtonStyle::Primary)) {
        similarTo_ = selected_;
        sortKey_ = library::SortKey::Similarity;
        sortDescending_ = true;
    }
    actions.removeFromLeft(t.scaled(4.0f));

    if (ui.button(ui.id("librarian.play"), actions.removeFromLeft(t.scaled(56.0f)), "play")
        && engine_ && loaded_)
        engine_->startPreview(loaded_, 0.0);
    actions.removeFromLeft(t.scaled(4.0f));

    if (ui.button(ui.id("librarian.topatch"), actions.removeFromLeft(t.scaled(70.0f)),
                  "to patch") && onSendToPatch)
        onSendToPatch(selected_);

    column.removeFromTop(t.scaled(4.0f));

    Rect processing = column.removeFromTop(t.scaled(22.0f));
    const auto process = [&](const char* label, const char* suffix, bool normalise, bool trim) {
        if (!ui.button(ui.id(std::string("librarian.") + label),
                       processing.removeFromLeft(t.scaled(78.0f)), label))
            return;

        const std::string written = writeProcessedCopy(suffix, normalise, trim);
        if (written.empty()) {
            ui.notify("could not write the copy", t.danger, 4.0f);
        } else {
            ui.notify("wrote " + pathLeaf(written), t.accent, 3.0f);
            openFolder(folder_);   // picks the new file up
        }
        processing.removeFromLeft(t.scaled(4.0f));
    };

    process("normalise", "-norm", true, false);
    process("trim", "-trim", false, true);
    process("both", "-clean", true, true);

    if (ui.isHot(ui.id("librarian.normalise")))
        ui.setTooltip("Writes a new file beside this one. The original is never touched.");
}

// ---------------------------------------------------------------------------

void LibrarianView::render(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();

    if (wizard_.active()) { wizard_.render(ui, bounds); return; }

    ui.draw().addRectFilled(bounds, t.background);

    Rect area = bounds;
    drawToolbar(ui, area);
    drawFilters(ui, area);

    drawDetail(ui, area.removeFromRight(detailWidth()));
    drawList(ui, area);
}

} // namespace acm::ui
