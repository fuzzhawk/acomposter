# acomposter

An AV patching and performance environment for Windows 10. Node-based patcher,
sample players and loopers, mixers and crossfaders, VST2 hosting for both 32-
and 64-bit plugins, a metasurface that re-poses the whole patch with one
gesture, a stem player with per-stem effect racks, a control surface that also
serves itself to a tablet, and a librarian that will analyse, classify, slice
and rebuild a folder of samples.

Built with no third-party libraries at all. Direct3D 11, Win32, WASAPI and
ASIO are the only things it stands on; the interface, the font rasteriser, the
audio codecs, the JSON, and both the VST2 and ASIO hosts are all in this
repository.

```
git clone https://github.com/fuzzhawk/acomposter
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

## What it does

### Node-based patcher

Drag from any output to any input. Cables are coloured by the source node's
category so a patch reads as shape and colour before you read a single label.

Feedback is legal. The scheduler detects cycles during its topological sort and
routes the back edges through one-block delay buffers, so a looper feeding its
own input resolves to a block of latency instead of a deadlock. Feedback cables
are drawn in amber with a marker at their midpoint, and the status bar counts
them, because each one costs you latency and you should know where they are.

Live patching never takes a lock. Every edit compiles a fresh immutable render
schedule which is handed to the audio thread through an atomic pointer store;
nodes you delete are held back until the engine's block counter proves the audio
thread has moved past them.

### Sample player

Two playback engines in one node:

- **Locked** derives the read position directly from the transport's musical
  position, every sample. The loop is squeezed to a whole number of beats and
  cannot drift, however long the set runs.
- **Free** runs on its own speed and pitch, for one-shots, stabs and layers you
  want to drift on purpose.

Forward, reverse and ping-pong; equal-power crossfade across the loop join;
trigger quantised to the beat or the bar; loop points read from a WAV file's
`smpl` chunk when it has one, and tempo from an `acid` chunk or the file name.

### Looper

One-button workflow: record, play, overdub. The first pass sets the loop length,
rounded to a beat grid if you have set one, so a slightly early punch still lands
in time. Feedback below 1 makes the take decay as you layer. Halve and double
without re-recording. Punch fades are configurable so the edits do not click.

### Mixer and crossfader

Mixers come in 4, 8 and 16 stereo channels with gain, pan, mute, solo and
per-channel metering. The crossfader is its own node because it is the most
performed control in the application: four curve laws (constant power, linear,
constant gain, transition), per-side trim, and hard cut buttons independent of
the fader.

### VST2, 32-bit and 64-bit

32-bit plugins cannot be loaded into a 64-bit process, so they are not. Each one
runs inside a helper process of its own architecture and is driven over a
shared-memory mailbox. 64-bit plugins load in-process by default, or through the
same bridge when you want the crash isolation.

Scanning is always out of process - probing is where plugins die, and a bad one
should cost you a helper, not your set. Results are cached by path, size and
modification time.

A plugin's parameters are mirrored as ordinary acomposter parameters, which is
what lets the metasurface interpolate a reverb's decay exactly as it does a
crossfader position.

acomposter ships no Steinberg source and does not need the VST2 SDK. See
[docs/VST2.md](docs/VST2.md).

### Metasurface

Set the patch up how you want it and capture a snapshot. Do it again somewhere
else. Now drag the cursor across the square between them and the whole patch
morphs.

Blending happens in *normalised* parameter space, which is what makes it
musical: a filter cutoff skewed so 1 kHz sits at the midpoint sweeps
logarithmically rather than crawling through the bottom two octaves. Stepped
parameters - choices, switches, playback direction - snap to whichever snapshot
currently dominates, because there is no meaningful midpoint between "forward"
and "reverse".

Three weighting modes:

| mode | behaviour |
| --- | --- |
| inverse distance | every snapshot pulls, strength `1/d^power`. Smooth and forgiving. The default. |
| radial basis | Gaussian falloff. Snapshots stay local, the surface grows distinct regions with soft borders. |
| nearest | winner takes all. A hard Voronoi, for scene switching rather than morphing. |

The coloured territories behind the points are the influence field: before you
move anything, you can see which snapshot owns where.

Any parameter can be excluded from the surface, so you can freeze one deck's
pitch while everything else morphs around it. And the cursor's movement can be
recorded and replayed, optionally locked to the transport so a gesture repeats
every N beats forever.

### Stem player

Load a song's stems, mark it up into named sections by bar, and perform it.
Every stem is the whole song; a section is a bar range into it. Looping one
means reading each stem at that range while the transport runs underneath, so
the loop is derived from the grid rather than accumulated and cannot drift
however long the set runs.

Asking for another section queues it. The current loop plays out and the change
lands on the boundary, so you commit to the next section whenever you notice you
want it and it arrives in time - the AudioMulch behaviour, rather than a clip
launcher that cuts. Launch quantise can be relaxed to the next bar, the next
beat, or immediate. The section parameter is stepped, so a metasurface blend
snaps to a section instead of trying to average two.

Eight stems, sixteen sections, one stereo output per stem plus a summed pair.

Each stem carries a **tag** - drums, bass, vocals, whatever the palette holds -
and the tag decides where the stem comes out. Tagging a file routes it, so the
same tags always land on the same outputs whatever order the stems were dropped
in and whatever the exporter called them. The routing matrix ("routing >" on the
node) shows the whole assignment at once and lets any stem be forced to any
output when a song wants something the tags would not do.

### Stem browser

The Stems tab is where a song is prepared rather than performed: drop the folder
an exporter produced, and every file in it is auditioned, tagged from its name,
and laid out ready to load as a set. Tags are editable in place - name, output
slot and colour - and a stem can be sent to a player, to a project, or straight
onto the canvas.

### Per-stem effect racks

Each stem output can carry its own chain of plugins, built from the inspector:
press the + beside a stem, pick a plugin, and it is created, placed and wired
into the end of that stem's chain. Removing one closes the gap so the audio
keeps flowing. "tidy" lays every rack out in rows beside the player.

The racks are ordinary nodes wired in series rather than plugin instances
hidden inside the stem player. That is deliberate. It reuses the whole VST host
- editors, 32-bit bridging, state, the scanner - and it means the colour
engine's existing addressing reaches these plugins with no changes at all. They
are visible on the canvas, which for a patcher is arguably where they belong,
and you can still re-patch them by hand if a song wants something the automatic
wiring would not do.

A rack can be **saved under a name and put back on any stem**, plugin state and
all - a chain that arrives at its defaults is a list of plugin names, not a
sound. Loading one tears the existing rack down first, because applying a preset
means the stem sounds like the preset rather than like the preset stacked on
whatever was there, and any plugin the preset names that this machine does not
have is reported rather than quietly skipped. A rack can also be copied from one
stem to another in two presses. Right-click a stem's output port on the canvas
to open that rack.

Tags carry a **default rack**, so tagging a stem "drums" can offer to build the
chain that always goes on drums. Offered rather than done: loading four plugins
takes a moment, and a mistagged file should not silently instantiate them.

### Colour

One knob that moves a whole effect chain between two captured states, through an
untouched middle. Red is tight, dry and low-heavy; blue is washed out,
high-passed and long-tailed; dead centre is the chain doing nothing.

The middle is a separately captured neutral, not the average of the two ends, so
a colour of zero is audibly unchanged whatever the ends are - which is what makes
the knob safe to leave alone mid-set - and an asymmetric preset still passes
through the sound the track actually has.

It drives plugins rather than containing them. **link every stem rack** adopts
every plugin hanging off every stem player in one press - build the racks, press
it, and the knob reaches the whole set. Or point it at one node at a time. Then
dial each end by ear and press "set red" or "set blue". Targets are discovered from whatever the plugin reports and presets
re-bind by name, reporting what they could not find rather than pointing at the
wrong control.

**On the four named plugins.** There is deliberately no hard-coded parameter map
for FabFilter Volcano, Pro-Q, FXpansion Bloom or 2CAudio Aether. None of them
publish a stable index map and several do not have one: Pro-Q exposes bands as
they are created, Volcano's modulation slots move with the routing, Bloom's
matrix is user-built. An index list would be wrong for a different band count
and would break silently on the next update, writing a reverb's decay into its
mix control in the middle of a set. Capturing the ends from the plugin's own
interface works with any of them, and keeps working.

**Colour presets** are the compromise between those two positions. A preset
names only parameters - no node, no plugin - and binds by name across everything
on the graph: a preset that drives "Frequency", "Mix" and "Decay" reaches
whichever filter and reverb are on the rack, and reaches them on all eight stems
at once. Four ship, one per instrument family. They will bind partially, they
say exactly which targets they could not find, and the ends are still worth
re-capturing by ear - which is what the capture buttons are for.

### Build and drop

**Build** is a momentary switch that takes a section apart. Held, it shortens the
stem loop step by step, runs a riser, pushes the colour toward blue and drops the
low end. Released, it returns on the next grid line rather than instantly, so
letting go slightly early still lands the drop in time.

**Drop** is the impact on the other side of it: three samples fired as one, on
the frame the build lets go. A drop is almost never one sound - a kick for the
weight, a crash or reverse cymbal for the air, and something sustained
underneath - and the three have to start together to read as one hit. Each layer
has its own gain, pitch, start offset and reverse.

The trigger travels as a *musical position* rather than a call, so the two nodes
work in whichever order the graph's schedule puts them: a drop node scheduled
before the build sees the event in the next block and offsets into it, landing on
the frame the build actually released. There is a manual trigger too, so the node
is useful on its own.

### Control surface

A patch of any size has hundreds of parameters and about eight that matter once
the set starts. The Control tab (`F2`) is where those eight go: knobs, faders,
buttons, X-Y pads and the metasurface itself, placed on a grid and sized in
cells rather than pixels - so a layout built on a laptop is still right on a
projector and on a tablet.

Each control drives a *list* of targets, each with its own range, which is the
whole difference between a remote control and a macro: one knob can open a
filter from 200 Hz to 8 kHz while it takes a reverb from dry to a third wet and
pushes a delay's feedback over only the top half of its travel. Binding is by
**learn**: arm it, move the control you want, and it is bound.

### Web control, for a tablet

Turn the server on in settings and the surface is served to any browser on the
network - the same layout, the same grid, the same behaviour under a finger. It
is written against the WebSocket specification directly, over the application's
own HTTP server, with no framework at either end: one page, no build step, no
dependency to keep current.

The protocol is deliberately small. The server sends the layout on connection
and whenever it changes, and a value as it moves; the client sends a value as it
moves. A value arriving from a tablet is applied with the same call the local
knob makes and echoed to every *other* client, never back to the one that sent
it, which would fight its own finger.

### Projects and songs

The library is a directory of small JSON files - one per song, one per project,
one per tagged asset - rather than a database. A song can be opened in a text
editor, hand-edited, diffed and put in version control beside the audio it
describes; a half-written file costs one entry rather than the library; two
machines merge by copying files. Nothing here is destructive: an entry
*references* audio by path and never moves, rewrites or owns it.

The **Songs** tab holds what a song is made of - its stems, its tempo, its key,
its sections, the patch it was built in. The **Projects** tab groups songs into
a set with a running order that can be reordered in place. A song can belong to
as many projects as it likes, because membership is recorded on the entry rather
than by where a file sits.

### File librarian

A folder of ten thousand one-shots is unusable by name. Names are inconsistent,
often wrong, and say nothing about what a sound is. So the librarian sorts and
filters by what the analysis found - length, brightness, pitch and confidence,
peak level, tags - and answers the question that actually comes up: *what else
in here sounds like this one*.

Analysis is a background scan: duration, peak and RMS, spectral centroid, eight
band energies, an envelope, and a pitch by normalised autocorrelation with an
octave correction and a physical sanity check, so a swept kick reports no pitch
rather than a confident wrong one. Each file gets a 3D spectral view - log-spaced
frequency rows over time, spun and tilted with the mouse - because the shape of a
hit is easier to recognise than its numbers.

Normalise and trim write a *sibling* file with a suffix. Nothing the librarian
offers can damage the folder it is pointed at.

Rows drag out of the window into any other application: find the sound, drag it
straight into the DAW.

### Analyse and rebuild

A folder of badly-named files, one screen at a time, into a folder that can be
worked with. Every file gets a guessed instrument, a proposed name and a tag,
each shown with the reason it was guessed, and nothing happens until it is
approved.

That asymmetry is the design. A classifier built on band energies and an
envelope will be wrong often enough that acting first and asking later would
mean a folder nobody can trust and no way to tell which parts were the machine's
fault. Approved files are *copied* to a new folder under their new names; the
originals stay exactly where they were.

It will also **slice** a loop into the hits it is made of, by spectral flux - the
rise in energy from one window to the next, which finds a snare landing on a
still-ringing kick where plain amplitude does not. Each slice is written out as
its own file and goes through the same analysis and classification as anything
else, so a sliced loop arrives already tagged.

### Audio output

WASAPI or ASIO, chosen in settings (`Ctrl+,`).

WASAPI needs no driver and shares the device with everything else on the
machine, but shared mode is stuck with the endpoint's mix format — which
Windows publishes as stereo on nearly every interface, however many outputs the
hardware actually has. If you can only see two channels of a twenty-channel
interface, that is why.

ASIO talks to the manufacturer's own driver and gets all of them. Pick the
driver, then choose how many channels the master bus renders and which of the
device's outputs they land on, so a stereo patch can come out of outputs 17-18
while something else uses 1-2. The driver's own control panel — buffer size,
clock source — is one button away, and when it asks to be reset acomposter
reopens the device by itself.

The ASIO host is written from the published specification, like the VST2 one;
no SDK is vendored or required. 64-bit drivers only. If ASIO cannot be opened,
acomposter falls back to WASAPI and says so rather than going silent.

### File browser

Down the left of the patch view: the folders acomposter writes to, the ones
samples usually live in, and every mounted drive, with a path field you can
paste into. Drag a file onto the canvas for a new player, or onto an existing
one to replace what it is holding. Double-click a patch to open it.

Drag a file *out of the window* and it becomes an ordinary Windows file drag, so
it can go straight into a DAW, an editor or a folder. A drag that ends inside the
window is still a drag to the canvas; leaving is what changes what it means.

---

## Getting started

The default patch gives you a sample player and a looper through a crossfader
into a mixer, with the looper already listening to the live input. Drop a WAV or
AIFF onto the canvas, or onto an existing player to replace its file.

| | |
| --- | --- |
| `Space` | play / pause |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` | new / open / save patch |
| `Ctrl+R` | capture a metasurface snapshot at the cursor |
| `Ctrl+,` | settings |
| `F1` `F2` `F3` `F4` | patch / control / plugins / stems |
| `F5` `F6` `F7` | projects / songs / library |
| `Ctrl+1` `Ctrl+2` `Ctrl+3` | toggle the browser, inspector and timeline |
| `Delete` | delete the selection |
| `Ctrl+D` | duplicate |
| `B` | bypass the selection |
| `Ctrl+A` | select all |
| middle-drag, or `Space`+drag | pan the canvas |
| wheel | zoom about the pointer |
| `Ctrl`+drag a node | snap to the grid |
| `Alt`+click a cable | cut it |
| double-click a node's name | rename in place |
| `Shift` while dragging a control | fine adjustment |
| double-click a control | reset to default |

On the metasurface: drag a snapshot to move it, `Alt`-click to recall it exactly,
right-click to delete it, double-click empty space to capture a new one there.

On a stem player: right-click an output port to open that stem's rack in the
inspector. `Shift`-click a tag in the stem browser to edit its name, output and
colour.

---

## Patches

Patches are JSON, on purpose. A performance tool whose documents are an opaque
blob is a tool you cannot rescue at 2am, and the format is small enough that
readability costs nothing:

```json
{
  "format": "acomposter-patch",
  "version": 1,
  "transport": { "bpm": 174, "timeSigNumerator": 4 },
  "nodes": [
    { "id": 1, "type": "sample.player", "name": "Deck A", "x": 60, "y": 80,
      "params": { "gain": 0, "sync": 1, "loopbeats": 4 },
      "state": { "samplePath": "C:\\breaks\\amen_174bpm.wav" } }
  ],
  "connections": [ { "from": 1, "fromPort": 0, "to": 3, "toPort": 0 } ],
  "metasurface": { "mode": 0, "snapshots": [ ] }
}
```

Node ids survive a save and load, because connections and metasurface snapshots
both address parameters as `(nodeId, paramIndex)`. Plugin state travels as
base64. A node type the build does not have degrades to a warning and keeps the
rest of the patch, rather than losing the lot.

Patches live in `Documents\acomposter\patches`; settings and the plugin scan
cache in `%APPDATA%\acomposter`. The library - songs, projects, tagged assets,
the tag palette, chain presets and colour presets - lives in
`Documents\acomposter\library`, one readable file per entry.

---

## Building

Requires CMake 3.20 and a C++20 compiler. Two configure passes, because the
32-bit bridge is a different architecture from everything else:

```
cmake -B build/x64 -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build/x64 --config Release

cmake -B build/x86 -A Win32 -DACOMPOSTER_BRIDGE_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/x86 --config Release
```

Then put `acomposter-bridge32.exe` beside `acomposter.exe`, which is where the
host looks for it.

It also cross-compiles from Linux with mingw-w64 - see
[docs/BUILDING.md](docs/BUILDING.md) - and CI does both on every push.

---

## Layout

```
src/core/       engine, graph, parameters, transport, JSON, file I/O
src/dsp/        interpolation, filters, fade laws, meters, FFT
src/audio/      WAV and AIFF codecs, sample buffers
src/nodes/      the node library
src/meta/       metasurface
src/control/    the played control surface: controls, targets, grid
src/library/    songs and projects, analysis, classification, slicing, presets
src/net/        HTTP and WebSocket servers, SHA-1, the control page
src/patch/      the .acp format
src/vst2/       clean-room VST2 ABI, in-process host, bridge client, scanner
src/bridge/     the helper process, built twice
src/gfx/        D3D11 renderer, draw list, GDI font atlas
src/ui/         immediate-mode framework, theme, views
src/platform/   window, WASAPI and ASIO backends, file dialogs, drag-out
tests/          831 checks, runnable on the build host
```

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) goes into how the threading and the
plugin bridge actually work.

---

## Status

Version 0.1.3. Everything described above is implemented, and the binaries build
clean under both MSVC and mingw-w64 with warnings as errors.

The engine, codecs, metasurface, patch format, section launching, colour
interpolation, analysis, classification, slicing, the library, the preset stores
and the WebSocket framing are covered by 831 checks that run on the build host.
The interface, the patcher, the stem browser, the librarian, the wizard, the
control surface, the file browser, drag and drop, and VST2 scanning and
instantiation have been driven end to end against the real binary. WASAPI has
been exercised on hardware.

Three things are honestly short of that:

- **ASIO has not been tested against a real driver.** It is written to the
  specification and its structure layouts are pinned by `static_assert`, but no
  interface has been in front of it. If it misbehaves, the driver name, its
  reported channel count and sample format, and what the status bar says are the
  useful things to report.
- **Chain presets have only been exercised against empty racks.** The capture,
  the store, the rebinding and the missing-plugin reporting are all tested; what
  has not been seen is a real plugin's state coming back out of one.
- **Web control has been driven from a desktop browser, not from an iPad.** The
  page, the protocol and the layout are tested; the touch behaviour on the device
  it was built for has not been.

The strip along the bottom of the patch view (`Ctrl+3`) is reserved for the
arrangement timeline and currently holds only a live bar ruler. It is laid out
now so the canvas, drop hit-testing and saved layout already account for it.
