# Architecture

How acomposter is put together, and why it is put together that way.

---

## The two threads

There are exactly two threads that matter, and they never block each other.

**The audio thread** belongs to WASAPI. It wakes on a device event, calls
`Engine::processInterleaved`, and that call must never allocate, lock, or wait.
Everything it touches was allocated before it started running.

**The message thread** does everything else: drawing, patch loading, plugin
scanning, editor windows. It owns the graph's structure and every object's
lifetime.

Where they meet, they meet through three mechanisms.

### Parameter atomics

Every `Parameter` holds one `std::atomic<float>`. The UI writes it, the audio
thread reads it. That is the whole story for the vast majority of state, and it
is why the metasurface can rewrite two hundred parameters per frame without any
coordination at all.

Nodes smooth what they read with a `SmoothedValue`, so a jumped parameter
becomes a ramp rather than a click.

### The published render schedule

Structural edits do not mutate anything the audio thread can see. Instead
`Graph::rebuildSchedule` compiles a fresh, immutable `RenderSchedule` — the
topological order, the buffer assignments, the fan-in mixes, the feedback taps —
and publishes it with a single atomic pointer store. The audio thread picks up
the new topology at the next block boundary. No lock, no dropout, no glitch.

The schedule owns its own buffers, so a swap is complete: there is no window in
which half the old plan and half the new one are live.

### The block counter, and deferred deletion

`AtomicResource<T>` is how a large object gets handed to the audio thread and,
crucially, taken back.

The engine bumps a monotonic block counter once per callback. When the message
thread retires an object — a superseded schedule, a deleted node, the sample a
player just replaced — it records the counter's value at that moment. The object
becomes unreachable once the counter has advanced past that value plus one: the
callback that might still have held a raw pointer has, provably, finished.
`collectGarbage()` then frees it on the message thread, where a free is harmless.

This is what lets you delete a node from a running patch without a lock and
without leaking.

---

## The graph

### Scheduling

`computeOrder` is an iterative depth-first search with the usual three-colour
marking. An edge into a grey node — one still on the traversal stack — is a back
edge, which means a cycle.

Back edges are not errors. They are removed from the ordering constraints and
routed through a dedicated delay buffer that is filled from the source's output
*after* all the steps have run. Next block, the destination reads it. A cycle
therefore resolves to exactly one block of latency per back edge, which the
status bar reports.

### Buffers

Each output port gets its own buffer. An input port with a single source whose
channel count already matches is handed that buffer directly — no copy at all.
An input with several sources, or one whose channel count differs, gets a scratch
buffer that the graph mixes into first, so every node sees exactly the channel
layout its port declared and no node has to write channel-adaptation code.

Upmix repeats source channels round-robin, so mono into stereo lands in both
sides. Downmix folds and normalises, so stereo into mono does not gain up 6 dB.

### Bypass

A bypassed node is not called at all; the graph copies input *k* to output *k*
for as many pairs as exist. That is both faster and more predictable than asking
every node to implement its own bypass.

---

## The metasurface

A snapshot is a sparse map from `(nodeId, paramIndex)` to a normalised value.
Snapshots sit at points on a unit square; the cursor's position determines a
weight per snapshot, and the weighted blend is written back into the graph.

Three things make this musical rather than merely functional:

**Normalised space.** A parameter's own skew is applied before blending. A cutoff
skewed so 1 kHz sits at 0.5 sweeps logarithmically between snapshots. Blending
raw values would spend most of the gesture in the bottom two octaves.

**Stepped parameters snap.** Choices, booleans and integers take their value from
whichever snapshot currently has the greatest weight. There is no halfway between
"forward" and "reverse", and pretending otherwise produces a parameter that
flickers between two values as you move.

**Coincidence is exact.** Sitting on a snapshot reproduces it exactly, not
approximately — otherwise recalling a saved scene would come back subtly wrong,
which is the kind of bug you only notice on stage.

Weights are renormalised per parameter over just the snapshots that actually
contain it, so a snapshot captured before a node existed does not drag that
node's parameters toward zero.

---

## The plugin bridge

### Why

A 32-bit DLL cannot be loaded into a 64-bit process. Not "should not" — cannot.
The only way to run 32-bit VST2 plugins from a 64-bit host is to put them in a
32-bit process and talk to it.

### The mailbox

Host and helper map one shared memory block:

```
0x00000000  control block   4 KB    command, status, arguments, transport, MIDI
0x00001000  audio in        1 MB    32 channels x 8192 frames of float
0x00101000  audio out       1 MB
0x00201000  data           32 MB    paths, strings, JSON, plugin state chunks
```

Two auto-reset events form the handshake: the host writes a command and signals
`request`, the helper executes it and signals `response`. One waiter released per
signal, which is exactly the ping-pong this needs.

### The layout problem

`BridgeProtocol.h` is compiled by two different compilers targeting two different
architectures, and both map the same bytes. Anything whose size or alignment
depends on pointer width would corrupt it silently.

So the control block uses only fixed-width types, every 8-byte field sits at an
8-byte-aligned offset by construction, padding is written out explicitly, and a
block of `static_assert`s on `offsetof` fails the build the moment the two
architectures disagree. Those asserts are checked in CI under both mingw targets.

Anything variable-length — strings, plugin state, the plugin description — goes
through the data area as bytes or JSON, never through the struct.

### Parameters ride along

A naive bridge does one IPC round trip per parameter change. With the metasurface
sweeping forty plugin parameters at 60 Hz, that is 2400 round trips a second.

Instead, parameter changes are accumulated on the audio thread and packed into
the data area alongside the audio block. The helper applies them immediately
before it processes. Cost: zero extra round trips.

### Failure

A helper that stops responding is noticed by the `Process` timeout. One late
block is tolerated — a plugin loading its own preset can take a moment — but
eight consecutive misses marks the bridge dead. A helper that has exited is
detected immediately via its exit code rather than waited on.

A dead plugin's node stays on the canvas and passes audio through untouched, with
the error visible on the box and a reload button next to it. Silence would be the
wrong failure mode in the middle of a set.

### Scanning

Scanning always uses the bridge, including for same-architecture plugins.
Probing is the single most likely moment for a plugin to crash, and on a first
run it happens hundreds of times. A crash there costs a helper process and one
entry in the failures list.

---

## Graphics

There is no UI toolkit, so:

**Fonts** are rasterised at startup with GDI's `GetGlyphOutline` in `GGO_GRAY8`
mode, which hands back an 8-bit coverage bitmap per glyph — exactly what an
alpha-blended atlas wants. Glyphs are shelf-packed into one texture that also
carries a small block of solid white texels.

**Every untextured shape samples that white block.** A rectangle, a cable and a
label are all the same draw call with the same texture and the same shader. A
whole frame is typically a handful of draw calls split only where the clip
rectangle changes.

**Anti-aliasing is geometric.** Filled shapes get a one-pixel feathered fringe
built into their vertices. No MSAA, no post-process, crisp at any DPI, free on
the GPU.

**The renderer** is deliberately small: inline HLSL compiled at startup, two
dynamic buffers rewritten each frame, scissor-rect clipping, and a WARP fallback
so the application still starts on a machine with no usable GPU driver.

---

## The interface

Immediate mode. The entire interface is rebuilt every frame from the engine's
state, which means there is no widget tree to keep in sync with the graph and no
way for the display to disagree with what is actually playing. Add a node from
the palette, from a patch file, or from a drag out of the browser, and it appears
the same way with no extra code on any of those paths.

Interaction uses the standard hot/active pair. `hot` is what the pointer is over;
`active` is what the pointer captured on press and keeps until release, which is
what lets a knob drag continue after the pointer leaves the knob.

Widgets take explicit rectangles rather than flowing in a layout, because almost
every surface here — a canvas, a mixer strip, a meter bridge — has geometry
dictated by the thing it is showing rather than by a layout algorithm.

---

## Audio I/O

Two backends behind one `platform::AudioDevice` interface, chosen in settings.
The application opens a device, hands over a callback and reads a status; which
backend is underneath is not something it knows.

### WASAPI

Shared mode, event driven. Shared rather than exclusive because acomposter is
meant to sit alongside whatever else is making noise, and because exclusive mode
fails outright on a device another application already holds — a bad way to
start a set.

The cost of shared mode is that the endpoint's mix format is fixed, and on
nearly every interface Windows publishes that as stereo no matter how many
outputs the hardware has. The channel count and offset in settings can only
route within those two.

### ASIO

The backend that reaches the rest of the outputs. It talks to the
manufacturer's own driver, which exposes every channel the interface has, and
runs at buffer sizes shared mode will not.

`AsioAbi.h` is a clean-room declaration of the interface, written from the
published specification for the same reason `Vst2Abi.h` is: Steinberg's SDK
cannot be redistributed, and the vtable order and structure layouts *are* the
interface. Sizes are pinned with `static_assert` so a mistake in a layout is a
compile error rather than a driver writing into the wrong bytes.

Two properties of ASIO shape the implementation. A driver is instantiated with
`CoCreateInstance` passing its CLSID as both the class id *and* the interface
id — there is no registered IID for `IAsio`. And the callbacks are bare function
pointers with no user-data argument, so the active device has to be reachable
from a global and a process can host exactly one driver at a time. Both are
properties of the interface rather than shortcuts.

Every output channel is requested from the driver, not just the ones the patch
reaches, because a channel with no buffer is one the driver may keep running
with stale contents. The unused ones are explicitly silenced each block;
whatever was in that half of the double buffer is the previous block, and
repeating it is an audible buzz.

A driver that asks to be reset — the user changed its buffer size in its own
control panel, or the interface was unplugged and put back — raises a flag that
the message thread acts on. Reopening the driver from inside its own callback
deadlocks it.

The header is 64-bit only. On x86 the SDK's methods specify no calling
convention, so they resolve to `__thiscall` under MSVC and cannot be described
portably; on x64 there is one convention and the question does not arise.
`acomposter.exe` is x64, so 64-bit drivers are what it loads.

A failed ASIO open falls back to WASAPI rather than leaving the program silent
behind an error nobody reads.

Capture, when enabled, runs on its own client with its own event. The two streams
have independent clocks and will drift, so capture frames reach the render thread
through a lock-free ring with half a second of slack. An under-run there yields
silence rather than a stall, which is the correct answer at startup and after a
glitch.

MMCSS (`AvSetMmThreadCharacteristics`, "Pro Audio") keeps the audio thread
scheduled against everything else on the machine.

The master bus ends in a soft `tanh` ceiling, on by default. A patch with a
runaway feedback loop should not be able to destroy a PA.

---

## Cross-node events, as musical positions

The build node releases and the drop node fires. The obvious implementation - a
call, or a flag the drop node polls - is wrong here, because the graph's
topological sort is free to schedule the two in either order and the correct
answer must not depend on which it chose.

So nothing is called. The build node *publishes the transport position it
released at*, and the drop node reads that position and works out where in its
own block the impact belongs. A drop node scheduled before the build simply sees
the event in the next block and offsets into it, landing on the frame the build
actually let go rather than a block early. A musical position is the one quantity
that means the same thing in both orders.

The offset is rounded rather than truncated. Truncating put every impact one
frame early, which is inaudible on its own and audible as a smeared transient
when three layers do it together.

The same channel carries the build's progress to the colour node, which is why
building pushes the colour toward blue without either node knowing about the
other.

## The control surface and its server

The surface is a model (`control::Surface`) that both the Control tab and the
web page render. There is deliberately only one: a second layout model for the
network client would be a second thing to keep in step, and it would drift.

Positions are grid cells rather than pixels, because a layout has to be right on
the machine it was built on, on a projector, and on a tablet, and a pixel layout
is right on exactly one of those.

The server runs on its own thread and never touches the graph. A value arriving
from a client is queued and applied on the message thread by the same call the
local knob makes, so there is exactly one path into a parameter and the audio
thread's assumptions hold whatever is driving it. The resulting layout goes back
out to every *other* client - never to the one that sent it, which would fight
its own finger.

HTTP, the WebSocket handshake, the frame codec and SHA-1 are all in `src/net`,
for the same reason everything else here is: a performance tool that cannot be
built five years from now because a dependency moved is not one worth building.

## The library

A directory of small JSON files - one per song, project or asset - rather than a
database. The trade is deliberate and worth restating because migrating it later
would mean migrating the user's own data: a corrupt or half-written file costs
one entry instead of the library, two machines merge by copying files, and every
record can be read and repaired in a text editor.

Membership is recorded on the entry rather than by where a file sits, so a song
belongs to as many projects as it likes. Running order is the exception: it lives
on the project's member list, because a song in three projects has three
positions and no field on the song could hold them.

Nothing in the library owns audio. An entry references a path; deleting an entry
deletes the entry.
