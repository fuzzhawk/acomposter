#include "WizardView.h"

#include "../audio/AudioFile.h"
#include "../core/FileIo.h"

#include <algorithm>
#include <cstdio>

namespace acm::ui {
namespace {

Colour tagColour(const library::Tag& tag) {
    return Colour{ static_cast<float>((tag.colour >> 16) & 0xFF) / 255.0f,
                   static_cast<float>((tag.colour >> 8) & 0xFF) / 255.0f,
                   static_cast<float>(tag.colour & 0xFF) / 255.0f, 1.0f };
}

} // namespace

void WizardView::initialise(Engine* engine, library::Library* library) {
    engine_ = engine;
    library_ = library;
}

void WizardView::begin(const library::FileIndex* index, std::string destinationFolder) {
    index_ = index;
    destination_ = std::move(destinationFolder);
    position_ = 0;
    sliceMode_ = false;
    nameBufferFor_ = -1;

    buildProposals();
    loadCurrent();
    active_ = !proposals_.empty();
}

void WizardView::buildProposals() {
    proposals_.clear();
    if (!index_) return;

    int index = 1;
    for (const library::IndexedFile& file : index_->files()) {
        if (!file.analysis.valid) continue;

        Proposal proposal;
        proposal.file = &file;
        proposal.guess = library::classify(file.analysis);
        proposal.proposedName = library::proposeName(file.analysis, proposal.guess.instrument,
                                                     index++);
        if (library_)
            proposal.tagId = library::tagForInstrument(library_->palette(),
                                                       proposal.guess.instrument);

        // Anything the classifier is confident about starts approved. The
        // wizard's job is to make the doubtful cases visible, not to make a
        // person press yes four hundred times.
        proposal.approved = proposal.guess.confidence >= 0.6f;

        proposals_.push_back(std::move(proposal));
    }
}

WizardView::Proposal* WizardView::current() {
    if (position_ < 0 || position_ >= static_cast<int>(proposals_.size())) return nullptr;
    return &proposals_[static_cast<std::size_t>(position_)];
}

void WizardView::step(int delta) {
    if (proposals_.empty()) return;

    position_ = clampValue(position_ + delta, 0, static_cast<int>(proposals_.size()) - 1);
    sliceMode_ = false;
    nameBufferFor_ = -1;
    loadCurrent();
}

void WizardView::loadCurrent() {
    loaded_.reset();
    slicePoints_.clear();

    const Proposal* proposal = current();
    if (!proposal || !proposal->file) return;

    std::string error;
    loaded_ = audiofile::load(proposal->file->path, &error);
    if (loaded_ && engine_) engine_->startPreview(loaded_, 0.0);
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

int WizardView::applyApproved(Ui& ui) {
    if (destination_.empty()) return 0;

    createDirectories(destination_);

    int written = 0;
    for (const Proposal& proposal : proposals_) {
        if (!proposal.approved || proposal.rejected || !proposal.file) continue;

        std::string error;
        const auto buffer = audiofile::load(proposal.file->path, &error);
        if (!buffer) continue;

        // Copied under the new name, never moved or renamed in place. Rebuilding
        // a sample library is done once, late, on a deadline; it has to be
        // impossible to lose anything doing it.
        std::string target = pathJoin(destination_, proposal.proposedName + ".wav");
        for (int attempt = 2; fileExists(target) && attempt < 1000; ++attempt) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "-%d", attempt);
            target = pathJoin(destination_, proposal.proposedName + suffix + ".wav");
        }

        if (!audiofile::writeWav(target, *buffer)) continue;
        ++written;

        // The tag goes on the copy, so the new folder arrives already sorted.
        if (library_ && !proposal.tagId.empty())
            library_->setTagForFile(target, proposal.tagId);
    }

    if (written > 0) {
        ui.notify("wrote " + std::to_string(written) + " files to "
                      + pathLeaf(destination_),
                  theme().accent, 5.0f);
        if (onFolderChanged) onFolderChanged();
    } else {
        ui.notify("nothing approved to write", theme().warning, 4.0f);
    }

    return written;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void WizardView::drawHeader(Ui& ui, Rect& area) {
    const Theme& t = theme();
    Rect bar = area.removeFromTop(t.scaled(32.0f));

    ui.draw().addRectFilled(bar, t.panelHeader);
    ui.draw().addRectFilled(Rect{ bar.left(), bar.bottom() - 1.0f, bar.width, 1.0f }, t.border);

    Rect row = bar.deflated(t.smallPadding);

    if (ui.button(ui.id("wizard.close"), row.removeFromLeft(t.scaled(56.0f)), "close"))
        close();
    row.removeFromLeft(t.scaled(10.0f));

    int approved = 0;
    for (const Proposal& proposal : proposals_)
        if (proposal.approved && !proposal.rejected) ++approved;

    char summary[192];
    std::snprintf(summary, sizeof(summary), "%d of %d    %d approved    into %s",
                  position_ + 1, static_cast<int>(proposals_.size()), approved,
                  destination_.empty() ? "(nowhere)" : pathLeaf(destination_).c_str());
    ui.labelDim(row, summary);
}

void WizardView::drawCurrent(Ui& ui, Rect& area) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    Proposal* proposal = current();
    if (!proposal || !proposal->file) return;

    const library::Analysis& analysis = proposal->file->analysis;

    // -- what it is --------------------------------------------------------
    ui.label(area.removeFromTop(t.scaled(22.0f)), proposal->file->name, t.text, t.fontUiBold);
    area.removeFromTop(t.scaled(4.0f));

    Rect guessRow = area.removeFromTop(t.scaled(26.0f));

    const Rect badge = guessRow.removeFromLeft(t.scaled(88.0f));
    const bool sure = proposal->guess.confidence >= 0.6f;
    list.addRectFilled(badge, (sure ? t.accent : t.warning).withAlpha(0.25f), t.cornerRadius);
    list.addRect(badge, sure ? t.accent : t.warning, 1.0f, t.cornerRadius);
    list.addTextClipped(ui.font(t.fontUiBold), badge, t.text,
                        library::toString(proposal->guess.instrument),
                        DrawList::Align::Centre);

    guessRow.removeFromLeft(t.scaled(8.0f));

    // The reason, always. A guess a person cannot check is a guess they have to
    // audition, and auditioning four hundred files is the thing this replaces.
    char why[192];
    std::snprintf(why, sizeof(why), "%.0f%% - %s",
                  static_cast<double>(proposal->guess.confidence) * 100.0,
                  proposal->guess.reason.c_str());
    list.addTextClipped(ui.font(t.fontSmall), guessRow, t.textDim, why);

    area.removeFromTop(t.scaled(6.0f));

    // -- the new name ------------------------------------------------------
    Rect nameRow = area.removeFromTop(t.scaled(24.0f));
    ui.label(nameRow.removeFromLeft(t.scaled(60.0f)), "name", t.textFaint, t.fontSmall);

    if (nameBufferFor_ != position_) {
        nameBuffer_ = proposal->proposedName;
        nameBufferFor_ = position_;
    }
    if (ui.textField(ui.id("wizard.name"), nameRow, nameBuffer_))
        proposal->proposedName = nameBuffer_;

    area.removeFromTop(t.scaled(6.0f));

    // -- the tag -----------------------------------------------------------
    if (library_) {
        Rect tagRow = area.removeFromTop(t.scaled(22.0f));
        ui.label(tagRow.removeFromLeft(t.scaled(60.0f)), "tag", t.textFaint, t.fontSmall);

        for (const library::Tag& tag : library_->palette().tags()) {
            const float width = ui.font(t.fontSmall).textWidth(tag.name) + t.scaled(12.0f);
            if (tagRow.width < width) {
                if (area.height < t.scaled(26.0f)) break;
                area.removeFromTop(t.scaled(2.0f));
                tagRow = area.removeFromTop(t.scaled(22.0f));
                tagRow.removeFromLeft(t.scaled(60.0f));
            }

            const Rect chip = tagRow.removeFromLeft(width);
            tagRow.removeFromLeft(t.scaled(2.0f));

            const bool on = proposal->tagId == tag.id;
            bool hovered = false, held = false;
            if (ui.buttonBehaviour(ui.id("wizard.tag." + tag.id), chip, hovered, held))
                proposal->tagId = on ? std::string() : tag.id;

            const Colour colour = tagColour(tag);
            Colour fill = on ? colour.withAlpha(0.5f) : colour.withAlpha(0.12f);
            if (hovered) fill = fill.brightened(1.35f);
            list.addRectFilled(chip, fill, 2.0f);
            list.addTextClipped(ui.font(t.fontSmall), chip, on ? t.text : t.textFaint,
                                tag.name, DrawList::Align::Centre);
        }
    }

    area.removeFromTop(t.scaled(8.0f));

    // -- facts -------------------------------------------------------------
    char facts[192];
    std::snprintf(facts, sizeof(facts), "%.2f s    %s    %.0f Hz bright    %.1f dB",
                  analysis.durationSeconds,
                  analysis.noteName.empty() ? "unpitched" : analysis.noteName.c_str(),
                  analysis.centroidHz,
                  20.0 * std::log10(std::max(static_cast<double>(analysis.peak), 1.0e-6)));
    list.addTextClipped(ui.font(t.fontMono), area.removeFromTop(t.scaled(16.0f)),
                        t.textFaint, facts);
}

void WizardView::drawSimilarGroup(Ui& ui, Rect& area) {
    const Theme& t = theme();

    Proposal* proposal = current();
    if (!proposal || !proposal->file) return;

    ui.separator(area.removeFromTop(t.scaled(9.0f)));

    // Everything else that looks like this one. Approving them together is the
    // difference between a wizard that saves time and one that is four hundred
    // key presses in a different order.
    std::vector<Proposal*> alike;
    for (Proposal& other : proposals_) {
        if (&other == proposal || !other.file) continue;
        if (library::similarity(other.file->analysis, proposal->file->analysis)
            >= groupThreshold_)
            alike.push_back(&other);
    }

    Rect header = area.removeFromTop(t.scaled(22.0f));

    char caption[96];
    std::snprintf(caption, sizeof(caption), "%d others like this", static_cast<int>(alike.size()));
    ui.label(header.removeFromLeft(t.scaled(140.0f)), caption, t.textDim, t.fontUiBold);

    float threshold = groupThreshold_;
    if (ui.sliderNormalised(ui.id("wizard.threshold"), header.removeFromRight(t.scaled(110.0f)),
                            threshold, t.warning))
        groupThreshold_ = clampValue(threshold, 0.4f, 0.99f);
    if (ui.isHot(ui.id("wizard.threshold")))
        ui.setTooltip("How alike counts as the same kind of sound");

    if (alike.empty()) return;

    area.removeFromTop(t.scaled(3.0f));
    Rect actions = area.removeFromTop(t.scaled(22.0f));

    if (ui.button(ui.id("wizard.applyall"), actions.removeFromLeft(t.scaled(130.0f)),
                  "same tag for all", Ui::ButtonStyle::Primary)) {
        for (Proposal* other : alike) {
            other->tagId = proposal->tagId;
            other->guess = proposal->guess;
            other->approved = true;
            other->rejected = false;
            // Renamed to match the kind, keeping whatever number their own name
            // carried - so a group approval does not collapse eight distinct
            // takes into one name with seven collision suffixes.
            other->proposedName = library::proposeName(other->file->analysis,
                                                       proposal->guess.instrument, 0);
        }
        ui.notify("applied to " + std::to_string(alike.size()) + " files", t.accent, 3.0f);
    }
    actions.removeFromLeft(t.scaled(6.0f));

    if (ui.button(ui.id("wizard.rejectall"), actions.removeFromLeft(t.scaled(100.0f)),
                  "skip all these", Ui::ButtonStyle::Danger)) {
        for (Proposal* other : alike) {
            other->rejected = true;
            other->approved = false;
        }
    }

    // The first few by name, so the group is checkable rather than a count.
    area.removeFromTop(t.scaled(3.0f));
    for (std::size_t i = 0; i < alike.size() && i < 4; ++i) {
        if (area.height < t.scaled(18.0f)) break;
        ui.draw().addTextClipped(ui.font(t.fontSmall), area.removeFromTop(t.scaled(15.0f)),
                                 t.textFaint, "  " + alike[i]->file->name);
    }
    if (alike.size() > 4 && area.height >= t.scaled(18.0f)) {
        ui.labelDim(area.removeFromTop(t.scaled(15.0f)),
                    "  and " + std::to_string(alike.size() - 4) + " more");
    }
}

void WizardView::drawSlices(Ui& ui, Rect& area) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    Proposal* proposal = current();
    if (!proposal || !loaded_) return;

    ui.separator(area.removeFromTop(t.scaled(9.0f)));
    Rect header = area.removeFromTop(t.scaled(22.0f));
    ui.label(header.removeFromLeft(t.scaled(60.0f)), "slice", t.textDim, t.fontUiBold);

    if (ui.button(ui.id("wizard.findslices"), header.removeFromLeft(t.scaled(90.0f)),
                  "find hits")) {
        library::SliceSettings settings;
        settings.sensitivity = sliceSensitivity_;
        slicePoints_ = library::findSlicePoints(*loaded_, settings);
        sliceMode_ = true;
    }
    header.removeFromLeft(t.scaled(6.0f));

    float sensitivity = (sliceSensitivity_ - 1.0f) / 2.0f;
    if (ui.sliderNormalised(ui.id("wizard.sensitivity"), header.removeFromLeft(t.scaled(110.0f)),
                            sensitivity, t.control))
        sliceSensitivity_ = 1.0f + sensitivity * 2.0f;

    if (!sliceMode_) return;

    // -- the waveform with its cut points ----------------------------------
    Rect strip = area.removeFromTop(t.scaled(56.0f));
    list.addRectFilled(strip, t.panelSunken, 2.0f);

    const auto& overview = loaded_->overview();
    if (!overview.minimum.empty()) {
        const float centre = strip.centre().y;
        const float half = strip.height * 0.45f;
        const int columns = static_cast<int>(strip.width);

        for (int x = 0; x < columns; ++x) {
            const auto bucket = static_cast<std::size_t>(
                static_cast<float>(x) / static_cast<float>(std::max(1, columns))
                * static_cast<float>(overview.minimum.size() - 1));
            const float low = overview.minimum[bucket];
            const float high = overview.maximum[bucket];
            list.addRectFilled(Rect{ strip.left() + static_cast<float>(x), centre - high * half,
                                     1.0f, std::max(1.0f, (high - low) * half) },
                               t.accent.withAlpha(0.6f));
        }
    }

    for (const std::int64_t point : slicePoints_) {
        const float x = strip.left() + strip.width
                      * static_cast<float>(point) / static_cast<float>(loaded_->frames());
        list.addRectFilled(Rect{ x, strip.top(), 1.5f, strip.height }, t.warning);
    }

    area.removeFromTop(t.scaled(4.0f));
    Rect sliceActions = area.removeFromTop(t.scaled(22.0f));

    char found[64];
    std::snprintf(found, sizeof(found), "%d hits", static_cast<int>(slicePoints_.size()));
    ui.labelDim(sliceActions.removeFromLeft(t.scaled(70.0f)), found);

    if (slicePoints_.size() > 1
        && ui.button(ui.id("wizard.writeslices"), sliceActions.removeFromLeft(t.scaled(110.0f)),
                     "write the slices", Ui::ButtonStyle::Primary)) {
        createDirectories(destination_);

        int written = 0;
        for (std::size_t i = 0; i < slicePoints_.size(); ++i) {
            const std::int64_t end = (i + 1 < slicePoints_.size()) ? slicePoints_[i + 1]
                                                                   : loaded_->frames();
            const auto slice = library::extractSlice(*loaded_, slicePoints_[i], end);
            if (!slice) continue;

            // Each slice is classified in its own right rather than inheriting
            // the loop's guess: the point of slicing a loop is that the pieces
            // are not all the same thing.
            const library::Analysis sliceAnalysis = library::analyse(*slice, proposal->file->name);
            const library::Classification guess = library::classify(sliceAnalysis);

            char name[128];
            std::snprintf(name, sizeof(name), "%s-%s-%02d",
                          pathStem(proposal->file->name).c_str(),
                          library::toString(guess.instrument), static_cast<int>(i) + 1);

            const std::string target = pathJoin(destination_, std::string(name) + ".wav");
            if (audiofile::writeWav(target, *slice)) {
                ++written;
                if (library_) {
                    const std::string tag = library::tagForInstrument(library_->palette(),
                                                                      guess.instrument);
                    if (!tag.empty()) library_->setTagForFile(target, tag);
                }
            }
        }

        ui.notify("wrote " + std::to_string(written) + " slices", t.accent, 4.0f);
        if (onFolderChanged) onFolderChanged();
    }
}

void WizardView::drawFooter(Ui& ui, Rect& area) {
    const Theme& t = theme();
    Rect bar = area.removeFromBottom(t.scaled(38.0f));

    ui.draw().addRectFilled(bar, t.panelHeader);
    ui.draw().addRectFilled(Rect{ bar.left(), bar.top(), bar.width, 1.0f }, t.border);

    Rect row = bar.deflated(t.smallPadding);

    if (ui.button(ui.id("wizard.previous"), row.removeFromLeft(t.scaled(50.0f)), "back"))
        step(-1);
    row.removeFromLeft(t.scaled(4.0f));

    Proposal* proposal = current();

    if (proposal) {
        const bool on = proposal->approved && !proposal->rejected;
        if (ui.button(ui.id("wizard.approve"), row.removeFromLeft(t.scaled(90.0f)),
                      on ? "approved" : "approve",
                      on ? Ui::ButtonStyle::Primary : Ui::ButtonStyle::Normal, on)) {
            proposal->approved = !on;
            proposal->rejected = false;
        }
        row.removeFromLeft(t.scaled(4.0f));

        if (ui.button(ui.id("wizard.skip"), row.removeFromLeft(t.scaled(60.0f)), "skip",
                      proposal->rejected ? Ui::ButtonStyle::Danger : Ui::ButtonStyle::Normal,
                      proposal->rejected)) {
            proposal->rejected = !proposal->rejected;
            proposal->approved = false;
        }
        row.removeFromLeft(t.scaled(4.0f));

        if (ui.button(ui.id("wizard.next"), row.removeFromLeft(t.scaled(60.0f)), "next"))
            step(1);
        row.removeFromLeft(t.scaled(4.0f));

        if (ui.button(ui.id("wizard.audition"), row.removeFromLeft(t.scaled(60.0f)), "play")
            && engine_ && loaded_)
            engine_->startPreview(loaded_, 0.0);
    }

    if (ui.button(ui.id("wizard.write"), row.removeFromRight(t.scaled(150.0f)),
                  "write the approved files", Ui::ButtonStyle::Primary))
        applyApproved(ui);
}

void WizardView::render(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();
    if (!active_) return;

    ui.draw().addRectFilled(bounds, t.background);

    Rect area = bounds;
    drawHeader(ui, area);
    drawFooter(ui, area);

    if (proposals_.empty()) {
        ui.labelDim(area, "nothing analysed to work through", DrawList::Align::Centre);
        return;
    }

    Rect column = area.deflated(t.padding);
    drawCurrent(ui, column);
    drawSimilarGroup(ui, column);
    drawSlices(ui, column);
}

} // namespace acm::ui
