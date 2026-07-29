# Building acomposter

No dependencies to fetch, vendor or install. CMake 3.20 and a C++20 compiler is
the whole list.

---

## Why two configure passes

acomposter ships three executables:

| binary | architecture | what it is |
| --- | --- | --- |
| `acomposter.exe` | x64 | the application |
| `acomposter-bridge64.exe` | x64 | scans plugins, and hosts 64-bit ones out of process when isolation is wanted |
| `acomposter-bridge32.exe` | **x86** | hosts 32-bit VST2 plugins |

The 32-bit bridge is a genuinely different architecture, which no single CMake
configure can produce alongside a 64-bit build. So there are two passes: one
normal, one with `ACOMPOSTER_BRIDGE_ONLY=ON` targeting x86.

All three must end up in the same directory. The host looks for its helpers
beside its own executable.

---

## MSVC (Visual Studio 2019 or later)

```bat
cmake -B build\x64 -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build\x64 --config Release

cmake -B build\x86 -A Win32 -DACOMPOSTER_BRIDGE_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build\x86 --config Release

copy build\x86\src\Release\acomposter-bridge32.exe build\x64\bin\
```

Run it from `build\x64\bin`.

---

## Cross-compiling from Linux with mingw-w64

Useful for CI and for checking that no MSVC-specific assumptions have crept in.

```sh
sudo apt-get install mingw-w64 ninja-build

cmake -B build/win64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/win64

cmake -B build/win32 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake \
  -DACOMPOSTER_BRIDGE_ONLY=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/win32

cp build/win32/src/acomposter-bridge32.exe build/win64/bin/
```

The mingw builds static-link the runtime, so the binaries have no GCC DLL
dependencies.

---

## Options

| option | default | effect |
| --- | --- | --- |
| `ACOMPOSTER_BRIDGE_ONLY` | `OFF` | build only the bridge helper. Used for the x86 pass. |
| `ACOMPOSTER_BUILD_TESTS` | `OFF` | build the test runner and register it with CTest. |
| `ACOMPOSTER_WERROR` | `OFF` | warnings as errors. CI uses this. |

---

## Tests

The engine, codecs, metasurface and patch format have no Windows dependency, so
the test suite builds and runs on the build host directly:

```sh
g++ -std=c++20 -O1 -I src tests/tests.cpp \
  src/core/Engine.cpp src/core/FileIo.cpp src/core/Graph.cpp \
  src/core/Json.cpp src/core/Node.cpp src/core/Parameter.cpp \
  src/core/Transport.cpp \
  src/audio/*.cpp src/nodes/*.cpp src/meta/*.cpp src/patch/*.cpp \
  src/vst2/PeArchitecture.cpp src/vst2/Vst2Plugin.cpp \
  -o acomposter_tests && ./acomposter_tests
```

Or through CMake on Windows:

```bat
cmake -B build\x64 -A x64 -DACOMPOSTER_BUILD_TESTS=ON
cmake --build build\x64 --config Release
ctest --test-dir build\x64 -C Release --output-on-failure
```

What is covered: graph routing, fan-in summing, feedback convergence, channel
adaptation, bypass, transport timing and tap tempo, parameter mapping and
skewing, WAV codec round trips at four bit depths, BPM detection, fade laws,
interpolation, metasurface weighting and exclusions, path playback, patch
round-trip idempotence, damaged-patch handling, base64, and PE architecture
detection.

What is not: the renderer, WASAPI, and plugin hosting against real plugins.
Those need real Windows and, for the last one, real plugins.

---

## Notes on the toolchains

**mingw and `wWinMain`.** MSVC infers a wide entry point from `UNICODE`; mingw's
startup code goes looking for a narrow `WinMain` unless it is passed `-municode`
at both compile and link time. That flag is on a separate interface target from
the shared flags, because the test runner is a console program with a plain
`main()`.

**WASAPI GUIDs.** `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT` and its PCM counterpart live
in a static library mingw does not ship, so both are written out as literal GUIDs
in `WasapiDevice.cpp`. Their values have been fixed since the format was defined.

**The bridge protocol.** `BridgeProtocol.h` is compiled for both architectures and
both map the same memory. It carries `static_assert`s on `offsetof` that fail the
build if the two compilers ever disagree about the layout. CI builds it under
both mingw targets specifically to keep that honest.

---

## CI

`.github/workflows/build.yml` runs two jobs on every push:

- **windows (msvc)** — builds both architectures, runs the tests, assembles the
  distribution, checks each binary's PE machine type, and uploads a zip. Tagged
  pushes get a release.
- **cross-compile (mingw-w64)** — builds both architectures from Linux with
  warnings as errors and runs the portable tests natively. A second opinion on
  the same tree, which catches what a single toolchain lets through.
