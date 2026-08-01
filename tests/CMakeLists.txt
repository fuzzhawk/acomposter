# The test runner covers the portable half of the engine, so it links against
# acomposter_core plus the two VST2 sources that have no Windows dependency
# (architecture detection reads a file header; it does not load anything).

add_executable(acomposter_tests
    tests.cpp
    ../src/vst2/PeArchitecture.cpp
    ../src/vst2/Vst2Plugin.cpp
    ../src/platform/DragOut.cpp
)

target_link_libraries(acomposter_tests PRIVATE acomposter_core acomposter_flags)
# DragOut's COM objects; the tests only exercise the path list, but the
# translation unit still has to link.
target_link_libraries(acomposter_tests PRIVATE ole32 uuid shell32)

# CTest finds the target regardless, but keeping the runner beside the binaries
# it exercises makes it easy to run by hand from a build tree.
set_target_properties(acomposter_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME acomposter COMMAND acomposter_tests)
