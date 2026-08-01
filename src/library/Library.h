// The library: songs, projects and tagged files, stored as one readable file
// per entry.
//
// The storage shape is a deliberate choice and worth stating, because migrating
// it later would mean migrating the user's own data. Everything is a directory
// of small JSON files - one per song, one per project, one per tagged asset -
// rather than a single database. That means:
//
//   * a song can be opened in a text editor, hand-edited, diffed, and put in
//     version control alongside the audio it describes,
//   * a corrupt or half-written file costs one entry rather than the library,
//   * two machines can merge a library by copying files, and
//   * nothing here is destructive: an entry *references* audio by path and
//     never moves, rewrites or owns it.
//
// A file can belong to as many songs and projects as it likes, because
// membership is recorded on the entry rather than by where the file sits.
#pragma once

#include "../core/Json.h"
#include "ChainPreset.h"
#include "TagPalette.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acm::library {

enum class EntryKind : int { Song = 0, Project, Asset };

const char* toString(EntryKind kind) noexcept;
EntryKind entryKindFromString(const std::string& text) noexcept;

struct Entry {
    std::string id;        // stable; also the file name
    EntryKind kind = EntryKind::Song;
    std::string name;

    // Free text the performer keeps with the entry. Notes for anything, lyrics
    // for a song - both plain text, both edited in place.
    std::string notes;
    std::string lyrics;

    // Absolute paths. Never copied, never moved: the library describes where
    // audio already lives rather than taking custody of it.
    std::vector<std::string> files;

    // Tag ids from the palette.
    std::vector<std::string> tags;

    // Songs a project contains, in album order. Only meaningful for projects.
    //
    // The order lives here, on the project, rather than as a number on the song.
    // A song belongs to as many projects as it likes, so a single position field
    // on the song cannot say where it sits in each of them - which is the whole
    // point of membership being non-destructive.
    std::vector<std::string> members;

    // Musical facts worth keeping next to the audio.
    double bpm = 0.0;
    std::string key;

    bool hasFile(const std::string& path) const;
    bool hasTag(const std::string& tagId) const;
};

class Library {
public:
    // Points the library at a directory and reads whatever is in it. Missing
    // directories are created; unreadable entries are skipped and counted
    // rather than failing the load, so one bad file cannot lock the rest out.
    bool open(const std::string& utf8RootDirectory, int* outSkipped = nullptr);

    const std::string& root() const noexcept { return root_; }
    bool isOpen() const noexcept { return !root_.empty(); }

    // -- entries -----------------------------------------------------------
    const std::vector<Entry>& entries() const noexcept { return entries_; }
    std::vector<const Entry*> entriesOfKind(EntryKind kind) const;

    const Entry* find(const std::string& id) const;
    Entry* find(const std::string& id);

    // Creates and writes an entry. Returns its id.
    std::string create(EntryKind kind, const std::string& name);
    bool remove(const std::string& id);

    // Writes one entry back to disk. Called after any edit; cheap enough that
    // there is no need for a dirty flag anyone could forget to set.
    bool save(const std::string& id) const;
    bool saveAll() const;

    // -- membership --------------------------------------------------------
    // Both sides are idempotent, because "add this file to that song" arriving
    // twice is a normal thing for a drag to do.
    bool addFile(const std::string& entryId, const std::string& path);
    bool removeFile(const std::string& entryId, const std::string& path);
    bool addMember(const std::string& projectId, const std::string& songId);
    bool removeMember(const std::string& projectId, const std::string& songId);
    // Moves a song within its project's running order. `delta` is in places;
    // moving past either end does nothing rather than wrapping.
    bool moveMember(const std::string& projectId, const std::string& songId, int delta);

    // Every entry that references a path - the answer to "what is this file
    // used by", which is the question that makes a non-destructive library
    // safe to reorganise.
    std::vector<const Entry*> entriesContaining(const std::string& path) const;

    // -- tags --------------------------------------------------------------
    TagPalette& palette() noexcept { return palette_; }
    const TagPalette& palette() const noexcept { return palette_; }
    bool savePalette() const;

    // The tag a file carries, from whichever asset entry describes it. Empty
    // when the file has not been tagged.
    std::string tagForFile(const std::string& path) const;
    bool setTagForFile(const std::string& path, const std::string& tagId);

    // -- chains ------------------------------------------------------------
    // Saved plugin racks live beside the tags because they are the other half
    // of the same idea: a tag says what a stem is, and the chain it names says
    // how that kind of stem gets treated. Carrying one folder carries both.
    ChainStore& chains() noexcept { return chains_; }
    const ChainStore& chains() const noexcept { return chains_; }

    // -- search ------------------------------------------------------------
    // Name, notes and file names, case insensitive. Tag filtering is by id.
    std::vector<const Entry*> search(const std::string& text, EntryKind kind) const;
    std::vector<const Entry*> withTag(const std::string& tagId) const;

private:
    std::string entryPath(const std::string& id) const;
    std::string makeUniqueId(const std::string& from) const;
    static Entry fromJson(const JsonValue& value);
    static JsonValue toJson(const Entry& entry);

    std::string root_;
    std::vector<Entry> entries_;
    TagPalette palette_;
    ChainStore chains_;
};

} // namespace acm::library
