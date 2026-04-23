# ---------------------------------------------------------------------------
# Engine-Relay (formerly Behavior Switchboard)
# SKSE plugin â€” runtime behavior graph augmentation for Skyrim SE/AE
# C++23, CommonLibSSE-NG
#
# Standalone:
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#         -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   cmake --build build --config Release
#
# Workspace:
#   add_subdirectory(Engine-Relay) from workspace CMakeLists.txt
#   (SHARED_INCLUDE_DIR is set by the workspace root)
#
# Dependencies:
#   CommonLibSSE-NG â€” set COMMONLIB_SSE_FOLDER env var, or place a clone at
#                     external/CommonLibSSE
#   vcpkg           â€” VCPKG_ROOT env var, or pass -DCMAKE_TOOLCHAIN_FILE
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.21)

set(AUTHOR_NAME  "CassieAndStuff")
set(PRODUCT_NAME "EngineRelay")

project(${PRODUCT_NAME} VERSION 0.1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# C++ standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

# ---------------------------------------------------------------------------
# Module path
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# CommonLibSSE
# ---------------------------------------------------------------------------
set(SKYRIM_SUPPORT_AE ON CACHE BOOL "" FORCE)
set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)

if(DEFINED ENV{COMMONLIB_SSE_FOLDER})
    set(COMMONLIB_PATH "$ENV{COMMONLIB_SSE_FOLDER}")
else()
    set(COMMONLIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/CommonLibSSE")
    if(NOT EXISTS "${COMMONLIB_PATH}/CMakeLists.txt")
        message(FATAL_ERROR
            "CommonLibSSE not found.\n"
            "Set COMMONLIB_SSE_FOLDER to a local clone, or run:\n"
            "  git submodule update --init --recursive")
    endif()
endif()

if(NOT TARGET CommonLibSSE)
add_subdirectory("${COMMONLIB_PATH}"
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/clib-build" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

# ---------------------------------------------------------------------------
# vcpkg packages
# ---------------------------------------------------------------------------
find_package(ryml CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
# Workspace builds receive SHARED_INCLUDE_DIR from the root CMakeLists.
# Standalone builds use the repo's own include/ (populated by Sync-Headers.ps1).
if(NOT DEFINED SHARED_INCLUDE_DIR)
    set(SHARED_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()

# ---------------------------------------------------------------------------
# Version resource
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY)

set(RESOURCE_FILE   "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
set(RESOURCE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/version.res")

add_custom_command(
    OUTPUT  ${RESOURCE_OUTPUT}
    COMMAND rc /fo ${RESOURCE_OUTPUT} ${RESOURCE_FILE}
    DEPENDS ${RESOURCE_FILE}
)
add_custom_target(EngineRelayResource ALL DEPENDS ${RESOURCE_OUTPUT})

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE BSB_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/header/*.h")
file(GLOB_RECURSE BSB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/*.cpp")
list(FILTER BSB_SOURCES EXCLUDE REGEX ".*Plugin\\.cpp$")

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR  ${AUTHOR_NAME}
    SOURCES ${BSB_HEADERS} ${BSB_SOURCES} ${RESOURCE_OUTPUT}
)

target_sources(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/Plugin.cpp"
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

# BSB_EXPORTS â€” marks BSB_API symbols as dllexport in this build.
# Consumers see dllimport automatically via BSB_API.h.
target_compile_definitions(${PROJECT_NAME} PRIVATE BSB_EXPORTS)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PLUGIN_LOG_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()

target_precompile_headers(${PROJECT_NAME} PRIVATE
    "${SHARED_INCLUDE_DIR}/PCH.h"
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        "${SHARED_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/header"
    PUBLIC
        # Engine-Relay's public API headers â€” available to workspace consumers
        # that link against EngineRelay (e.g. True-Cinematics).
        "${SHARED_INCLUDE_DIR}"
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ryml::ryml
)

# ---------------------------------------------------------------------------
# Deployment
# ---------------------------------------------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/automaticGameFolderOutput.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/copyOutputs.cmake")

automaticGameFolderOutput()

# ---------------------------------------------------------------------------
# Behavior source compilation (YAML -> HKX via HKBuild)
# Only enabled when HKBuild is available (workspace builds or explicit path).
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/behavior_src/CMakeLists.txt")
    if(TARGET HKBuild OR DEFINED HKBUILD_EXE)
        add_subdirectory(behavior_src)
        message(STATUS "EngineRelay: behavior compilation enabled (HKBUILD_EXE=${HKBUILD_EXE})")
    else()
        message(STATUS "EngineRelay: HKBuild not found - behavior compilation skipped.")
        message(STATUS "  Set HKBUILD_EXE or build from the workspace root to enable.")
    endif()
endif()
