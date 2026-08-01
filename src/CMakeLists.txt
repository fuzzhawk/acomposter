# ---------------------------------------------------------------------------
# acomposter_core
#
# The portable half: engine, nodes, metasurface, codecs, patch format. It builds
# with nothing but a C++20 compiler, which is what lets the test suite run on the
# build host rather than only on the target.
# ---------------------------------------------------------------------------

set(ACOMPOSTER_CORE_SOURCES
    core/Engine.cpp
    core/FileIo.cpp
    core/Graph.cpp
    core/Json.cpp
    core/Node.cpp
    core/Parameter.cpp
    core/Transport.cpp

    audio/AudioFile.cpp
    audio/SampleBuffer.cpp

    control/Surface.cpp
    dsp/Fft.cpp
    library/AudioAnalysis.cpp
    library/FileIndex.cpp
    library/ChainPreset.cpp
    library/Library.cpp
    library/TagPalette.cpp
    nodes/BuildNode.cpp
    nodes/ColorNode.cpp
    nodes/CrossfaderNode.cpp
    nodes/DropNode.cpp
    nodes/IoNodes.cpp
    nodes/LooperNode.cpp
    nodes/MixerNode.cpp
    nodes/NodeFactory.cpp
    nodes/SamplePlayerNode.cpp
    nodes/StemPlayerNode.cpp
    nodes/UtilityNodes.cpp

    meta/Metasurface.cpp
    patch/Patch.cpp
)

add_library(acomposter_core STATIC ${ACOMPOSTER_CORE_SOURCES})
target_include_directories(acomposter_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(acomposter_core PUBLIC acomposter_flags)

# ---------------------------------------------------------------------------
# acomposter_vst2
#
# Shared by the host application and both bridge helpers, so it must build for
# x86 as well as x64.
# ---------------------------------------------------------------------------

set(ACOMPOSTER_VST2_SOURCES
    vst2/NativeVst2Plugin.cpp
    vst2/PeArchitecture.cpp
    vst2/PluginEditorWindow.cpp
    vst2/Vst2Plugin.cpp
)

add_library(acomposter_vst2 STATIC ${ACOMPOSTER_VST2_SOURCES})
target_include_directories(acomposter_vst2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(acomposter_vst2 PUBLIC acomposter_core acomposter_flags)
target_link_libraries(acomposter_vst2 PUBLIC user32 gdi32 ole32 shell32)

# ---------------------------------------------------------------------------
# The bridge helper
#
# Built for whichever architecture this configure pass targets. The host build
# produces acomposter-bridge64.exe; a second configure pass with the i686
# toolchain and ACOMPOSTER_BRIDGE_ONLY produces acomposter-bridge32.exe.
# ---------------------------------------------------------------------------

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ACOMPOSTER_BRIDGE_NAME acomposter-bridge64)
else()
    set(ACOMPOSTER_BRIDGE_NAME acomposter-bridge32)
endif()

add_executable(acomposter_bridge WIN32 bridge/BridgeMain.cpp)
set_target_properties(acomposter_bridge PROPERTIES OUTPUT_NAME ${ACOMPOSTER_BRIDGE_NAME})
target_link_libraries(acomposter_bridge PRIVATE acomposter_vst2 acomposter_flags acomposter_wide_entry)

if(ACOMPOSTER_BRIDGE_ONLY)
    return()
endif()

# ---------------------------------------------------------------------------
# acomposter
# ---------------------------------------------------------------------------

set(ACOMPOSTER_APP_SOURCES
    main.cpp

    app/Application.cpp

    core/AppPaths.cpp

    gfx/DrawList.cpp
    gfx/FontAtlas.cpp
    gfx/Renderer.cpp

    platform/AsioDevice.cpp
    platform/AudioDevice.cpp
    platform/FileDialog.cpp
    platform/WasapiDevice.cpp
    platform/Window.cpp

    ui/ControlView.cpp
    ui/LibrarianView.cpp
    ui/LibraryView.cpp
    ui/MetasurfaceView.cpp
    ui/Panels.cpp
    ui/StemBrowserView.cpp
    ui/PatcherView.cpp
    ui/Theme.cpp
    ui/Ui.cpp

    vst2/BridgedVst2Plugin.cpp
    vst2/PluginManager.cpp
    vst2/VstNode.cpp
)

add_executable(acomposter WIN32 ${ACOMPOSTER_APP_SOURCES})
target_link_libraries(acomposter PRIVATE acomposter_vst2 acomposter_core acomposter_flags acomposter_wide_entry)

target_link_libraries(acomposter PRIVATE
    d3d11           # renderer
    dxgi
    d3dcompiler     # shaders are compiled at start-up
    gdi32           # glyph rasterisation for the font atlas
    user32
    shell32         # drag and drop, known folders
    ole32           # COM, for WASAPI and the shell dialogs
    oleaut32
    uuid
    avrt            # MMCSS, to keep the audio thread scheduled
    winmm
    advapi32        # registry, to find the VST2 plugin folders
)

# The bridge helper has to sit beside the application, because that is where the
# host looks for it at run time.
add_dependencies(acomposter acomposter_bridge)

set_target_properties(acomposter acomposter_bridge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
)
