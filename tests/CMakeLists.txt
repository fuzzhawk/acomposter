# The test runner covers the portable half of the engine, so it links against
# acomposter_core plus the two VST2 sources that have no Windows dependency
# (architecture detection reads a file header; it does not load anything).

add_executable(acomposter_tests
    tests.cpp
    ../src/vst2/PeArchitecture.cpp
    ../src/vst2/Vst2Plugin.cpp
)

target_link_libraries(acomposter_tests PRIVATE acomposter_core acomposter_flags)

set_target_properties(acomposter_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

add_test(NAME acomposter COMMAND acomposter_tests)
