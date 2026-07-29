# VST2 support and the SDK question

acomposter hosts VST 2.4 plugins. It contains no Steinberg source code, and you
do not need the VST2 SDK to build it.

## Why this needs explaining

Steinberg withdrew the VST2 SDK in 2018. It is no longer distributed, no new
licences are granted, and the licence attached to the copies that exist forbids
redistribution. A project that `#include`d `aeffect.h` could not ship a buildable
tree, and could not be built by anyone who had not obtained the SDK before it was
withdrawn.

Meanwhile there are twenty-odd years of VST2 plugins on people's machines, many
with no VST3 equivalent, and a performance tool that cannot load them is
considerably less useful.

## What this repository actually contains

`src/vst2/Vst2Abi.h` is a clean-room description of the VST 2.4 *binary
interface*: the layout of the `AEffect` struct, the opcode numbers, the flag
values, and the shape of the callbacks. It was written from published knowledge
of that interface, not copied from Steinberg's headers.

This is a description of an interface that every VST2 plugin already implements
and publishes, in the same sense that a description of a file format is not a
copy of the software that writes it. The names in this file are our own where
they can be; where a name is part of the ABI's public vocabulary (`effOpen`,
`audioMasterGetTime`), it is used because that is what the interface *is*.

The one rule that matters technically: the declarations must produce the same
memory layout the plugin's own compiler produced. That means matching field order
and letting natural alignment do its work — the original has no packing pragmas,
so neither does this.

## What this means for you

**Building:** nothing extra. `cmake` and a compiler.

**Distributing binaries:** acomposter's own licence applies; there is no
Steinberg code in the tree to encumber it.

**The "VST" trademark:** VST is a Steinberg trademark. This project describes
itself as hosting "VST2 plugins" descriptively. It is not a licensed VST host,
it carries no Steinberg branding, and it is not endorsed by or affiliated with
Steinberg.

**If you hold a VST2 SDK licence** and would rather build against the real
headers, the ABI is identical; substituting them is a header swap.

## What is and is not implemented

Implemented:

- `effOpen` / `effClose`, sample rate and block size, `effMainsChanged`,
  `effStartProcess` / `effStopProcess`
- `processReplacing` (VST 2.4 in-place processing)
- Parameters: get, set, name, label, display
- Programs: get, set, indexed names, begin/end wrappers
- State: `effGetChunk` / `effSetChunk` when the plugin advertises
  `effFlagsProgramChunks`, and a parameter dump when it does not
- Editors: `effEditGetRect`, `effEditOpen`, `effEditClose`, `effEditIdle`, and
  `audioMasterSizeWindow`
- `effProcessEvents` for MIDI, with a note-off sweep on panic
- Host callbacks: version, sample rate, block size, `audioMasterGetTime` with
  full transport state, vendor and product strings, `audioMasterAutomate`,
  `audioMasterUpdateDisplay`, `audioMasterSizeWindow`, `audioMasterCanDo`

Not implemented:

- `processDoubleReplacing`. Everything runs in 32-bit float.
- The accumulating pre-2.4 `process` call. A plugin without
  `effFlagsCanReplacing` is rejected with a clear message rather than silently
  producing nothing.
- Offline and variable I/O processing (`effOfflineRun`, `effProcessVarIo`).
- Shell plugins (`effShellGetNextPlugin`). A shell's sub-plugins are not
  enumerated; only the shell itself is seen.
- Speaker arrangement negotiation. The patcher works in stereo and adapts around
  whatever channel count a plugin declares.

`audioMasterCanDo` answers honestly. Claiming support acomposter does not have
makes plugins take code paths that then fail in less obvious ways, which is worse
than a plugin knowing up front what it cannot rely on.

## Editors are separate windows

acomposter draws its own interface with Direct3D and has no child-HWND concept,
so a plugin editor gets its own floating top-level window rather than being
embedded in the patcher.

This is also what makes the bridged case work without cross-process
reparenting: the helper process owns its editor window outright, and Windows
handles z-order and focus as it would for any other application.

## VST3

Not supported. VST3 is a different, much larger interface, and its SDK is
available under the GPL or a commercial licence — which is a licensing decision
for this project rather than a technical obstacle. If it happens, it will be a
separate host behind the same `Vst2Plugin`-shaped interface that already exists,
which is why that interface is abstract.
