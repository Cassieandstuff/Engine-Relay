cmake_minimum_required(VERSION 3.21)

set(AUTHOR_NAME "EngineRelay")
set(PRODUCT_NAME "EngineRelay")

project(${PRODUCT_NAME} VERSION 0.1.0.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(cmake/lib/copyOutputs.cmake)
include(cmake/lib/automaticGameFolderOutput.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)

# ── Version resource ─────────────────────────────────────────────────────────

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    @ONLY)

set(RESOURCE_FILE   ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
set(RESOURCE_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/version.res)

add_custom_command(
    OUTPUT  ${RESOURCE_OUTPUT}
    COMMAND rc /fo ${RESOURCE_OUTPUT} ${RESOURCE_FILE}
    DEPENDS ${RESOURCE_FILE}
)
add_custom_target(Resource ALL DEPENDS ${RESOURCE_OUTPUT})

# ── CommonLibSSE ─────────────────────────────────────────────────────────────

set(SKYRIM_SUPPORT_AE ON CACHE BOOL " " FORCE)
set(BUILD_TESTS       OFF CACHE BOOL " " FORCE)

if(DEFINED ENV{COMMONLIB_SSE_FOLDER})
    set(COMMONLIB_PATH "$ENV{COMMONLIB_SSE_FOLDER}")
    add_subdirectory("${COMMONLIB_PATH}"
        ${CMAKE_CURRENT_BINARY_DIR}/_deps/clib-build EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(CommonLibSSE
        GIT_REPOSITORY https://github.com/powerof3/CommonLibSSE.git
        GIT_TAG        aad6fb7dc9cd39754a28742b3b709ae29dd20843
        SOURCE_DIR     "${CMAKE_CURRENT_BINARY_DIR}/_deps/commonlibsse-src"
        BINARY_DIR     "${CMAKE_CURRENT_BINARY_DIR}/_deps/clib-build"
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(CommonLibSSE)
    set(COMMONLIB_PATH "${commonlibsse_SOURCE_DIR}")
endif()

get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)
include(${CMAKE_SOURCE_DIR}/cmake/CommonLibSSE.cmake)

# ── vcpkg packages ───────────────────────────────────────────────────────────

find_path(CLIB_UTIL_INCLUDE_DIRS     "ClibUtil/utils.hpp"     REQUIRED)
find_path(CLIB_UTILS_QTR_INCLUDE_DIRS "ClibUtilsQTR/utils.hpp" REQUIRED)
find_package(ryml CONFIG REQUIRED)

# ── Source files ─────────────────────────────────────────────────────────────

file(GLOB_RECURSE ER_SOURCES "src/cpp/*.cpp")
list(FILTER ER_SOURCES EXCLUDE REGEX ".*Plugin\\.cpp$")

file(GLOB_RECURSE ER_HEADERS
    "src/header/*.h"
    "include/**/*.h"
    "include/**/*.hpp"
)

file(GLOB_RECURSE extra_sources ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
set(ALL_SOURCES ${ER_SOURCES} ${extra_sources})

source_group(
    TREE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ER_HEADERS} ${ALL_SOURCES}
)

# ── EngineRelay plugin target ─────────────────────────────────────────────────

add_commonlibsse_plugin(
    ${PROJECT_NAME}
    AUTHOR ${AUTHOR_NAME}
    SOURCES ${ER_HEADERS} ${ALL_SOURCES}
)

target_sources(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/Plugin.cpp"
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

# Export ER_API symbols. Consumer plugins see dllimport via ER_API.h.
target_compile_definitions(${PROJECT_NAME} PRIVATE ER_EXPORTS)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        PLUGIN_LOG_DIR="${CMAKE_SOURCE_DIR}"
    )
endif()

target_precompile_headers(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
    ${CLIB_UTIL_INCLUDE_DIRS}
    ${CLIB_UTILS_QTR_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/header
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
    ryml::ryml
)

# Deploy DLL to MO2 mods folder on build (set SKYRIM_MODS_FOLDER env var).
if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(ER_MOD_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/Engine Relay")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${ER_MOD_FOLDER}/SKSE/Plugins"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${ER_MOD_FOLDER}/SKSE/Plugins/${PROJECT_NAME}.dll"
        COMMENT "Deploying EngineRelay.dll → ${ER_MOD_FOLDER}"
        VERBATIM
    )
endif()

automaticGameFolderOutput()

# ── HKBuild — C# YAML→HKX compiler ───────────────────────────────────────────
# Fetched at configure time into build/_deps/ (not tracked in the source tree).
# Publishes a single-file self-contained win-x64 exe into build/hkbuild_sc/.

include(FetchContent)
FetchContent_Declare(HKBuild
    GIT_REPOSITORY       https://github.com/Cassieandstuff/HKBuild.git
    GIT_TAG              a9a286216fbe0f5d2fa61de1723007db0c6ed946
    GIT_SUBMODULES_RECURSE TRUE
    SOURCE_DIR           "${CMAKE_CURRENT_BINARY_DIR}/_deps/hkbuild-src"
)
FetchContent_Populate(HKBuild)

set(_hkbuild_sc_dir "${CMAKE_BINARY_DIR}/hkbuild_sc")
set(HKBUILD_EXE "${_hkbuild_sc_dir}/HKBuild${CMAKE_EXECUTABLE_SUFFIX}"
    CACHE FILEPATH "Path to the built HKBuild executable" FORCE)

# Locate the .csproj — src/ subfolder or repo root.
if(EXISTS "${hkbuild_SOURCE_DIR}/src/HKBuild.csproj")
    set(_hkbuild_csproj "${hkbuild_SOURCE_DIR}/src/HKBuild.csproj")
elseif(EXISTS "${hkbuild_SOURCE_DIR}/HKBuild.csproj")
    set(_hkbuild_csproj "${hkbuild_SOURCE_DIR}/HKBuild.csproj")
else()
    message(WARNING "HKBuild.csproj not found — behavior YAML compilation disabled.")
    set(_hkbuild_csproj "")
endif()

if(_hkbuild_csproj)
    # Patch TargetFramework to net10.0 at configure time so NuGet restore sees it.
    # HKX2E (HKBuild's dependency) requires net10.0; the pinned csproj says net9.0.
    file(READ "${_hkbuild_csproj}" _csproj_content)
    string(REPLACE "<TargetFramework>net9.0</TargetFramework>"
                   "<TargetFramework>net10.0</TargetFramework>"
                   _csproj_content "${_csproj_content}")
    file(WRITE "${_hkbuild_csproj}" "${_csproj_content}")

    file(GLOB_RECURSE _hkbuild_sources
        "${hkbuild_SOURCE_DIR}/*.cs"
        "${_hkbuild_csproj}"
    )

    add_custom_command(
        OUTPUT  "${HKBUILD_EXE}"
        COMMAND dotnet publish "${_hkbuild_csproj}"
                -c Release
                -r win-x64
                --self-contained true
                -p:PublishSingleFile=true
                -o "${_hkbuild_sc_dir}"
        DEPENDS ${_hkbuild_sources}
        WORKING_DIRECTORY "${hkbuild_SOURCE_DIR}"
        COMMENT "Building HKBuild (single-file self-contained → hkbuild_sc/HKBuild.exe)"
        VERBATIM
    )

    add_custom_target(HKBuild DEPENDS "${HKBUILD_EXE}")

    file(TO_CMAKE_PATH "${HKBUILD_EXE}" _hkbuild_cmake_path)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        ER_HKBUILD_DEV_PATH="${_hkbuild_cmake_path}"
    )
    add_dependencies(${PROJECT_NAME} HKBuild)
endif()

# ── Behavior source compilation (YAML → HKX via HKBuild) ─────────────────────

if(DEFINED HKBUILD_EXE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/behavior_src/CMakeLists.txt")
    add_subdirectory("behavior_src")
endif()
