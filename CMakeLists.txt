cmake_minimum_required(VERSION 3.20)
project(WowRadio LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)

# Statically link the MSVC runtime so the DLL needs no VC++ redistributable on
# players' machines.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# WowRadio.dll: the radio engine. BASS is embedded as a resource and loaded from
# memory, so this is a single file with no external dependency besides Windows.
add_library(WowRadio SHARED
    src/main.cpp
    src/bass_loader.cpp
    src/resources.rc
    third_party/MemoryModule/MemoryModule.c)
target_include_directories(WowRadio PRIVATE src third_party/bass third_party/MemoryModule)
target_link_libraries(WowRadio PRIVATE shell32)
target_compile_definitions(WowRadio PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(WowRadio PROPERTIES OUTPUT_NAME "WowRadio" PREFIX "")
