// Reads a DLL's target architecture straight out of its PE header.
//
// This has to happen before any attempt to load: calling LoadLibrary on a
// 32-bit DLL from a 64-bit process fails with a misleading "not a valid Win32
// application", and doing it the other way round is worse. Parsing the header
// costs a couple of hundred bytes of file read and tells us definitively whether
// a plugin needs the bridge.
#pragma once

#include "Vst2Plugin.h"

#include <string>

namespace acm::vst2 {

// Returns Unknown when the file is missing, too short, or not a PE image.
Architecture readPeArchitecture(const std::string& utf8Path, std::string* error = nullptr);

// True when the file is a DLL rather than an executable. VST2 plugins are DLLs
// with a .dll extension; this catches the occasional stray .exe in a plugin
// folder before we try to load it.
bool isDynamicLibrary(const std::string& utf8Path);

} // namespace acm::vst2
