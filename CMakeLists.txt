cmake_minimum_required(VERSION 3.20)
project(KirbyMirrorRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Path to the platform core (git submodule).
set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp"
    CACHE PATH "Path to gbarecomp platform core (git submodule)")

# Opt this game into the data-only .gbamod package catalog (see
# ../gbarecomp/docs/MOD_PACKAGES.md). Must be set BEFORE add_subdirectory.
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL "" FORCE)

if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT does not point to a gbarecomp checkout: ${GBARECOMP_ROOT}")
endif()

# Pull in the platform core targets. This makes gbarecomp_runtime,
# gbarecomp_debug, gbarecomp_gba, gbarecomp_armv4t available as link targets.
add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)

# KirbyMirrorRecomp — the game binary. Links generated/*.cpp against the
# runtime libs.
file(GLOB KATAM_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")

set(KATAM_RUNTIME_SOURCES
    src/main.cpp
    src/mod_widescreen.cpp
    ${KATAM_GENERATED_SOURCES}
)

add_executable(KirbyMirrorRecomp ${KATAM_RUNTIME_SOURCES})

target_include_directories(KirbyMirrorRecomp PRIVATE
    "${GBARECOMP_ROOT}/src/armv4t"
    "${GBARECOMP_ROOT}/src/gba"
    "${GBARECOMP_ROOT}/src/runtime"
    "${GBARECOMP_ROOT}/src/debug"
    src
)

target_link_libraries(KirbyMirrorRecomp PRIVATE
    "-Wl,--start-group"
    gbarecomp_runtime
    gbarecomp_debug
    gbarecomp_gba
    gbarecomp_armv4t
    "-Wl,--end-group"
)

target_compile_definitions(KirbyMirrorRecomp PRIVATE
    GBARECOMP_WINDOW_TITLE="KirbyMirrorRecomp"
    GBARECOMP_DEFAULT_DEBUG_PORT=19853
    GBARECOMP_DEFAULT_GAME_CONFIG="game.toml"
)

# The built-in mod packages must live beside the executable: the runtime
# loads the catalog from <exe_dir>/mods.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mods/packages")
    add_custom_command(TARGET KirbyMirrorRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/mods"
                "$<TARGET_FILE_DIR:KirbyMirrorRecomp>/mods"
        COMMENT "Copying built-in mod packages next to the executable")
endif()
