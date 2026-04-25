# deployData.cmake — Deploy a plugin's Data/ folder to the MO2 mods folder.
#
# Usage (from a plugin's CMakeLists.txt):
#
#   include(${CMAKE_SOURCE_DIR}/cmake/lib/deployData.cmake)
#   deployData(
#       TARGET      MyPlugin
#       DATA_DIR    "SubMenu Framework"   # subfolder name under Data/
#       MOD_NAME    "SubMenuFramework"    # subfolder name under SKYRIM_MODS_FOLDER
#   )
#
# Convention: directories whose name starts with '_' are skipped.
# This lets you keep templates, notes, or scratch folders in Data/
# without them being deployed.

# -- Build-time script (invoked via cmake -P) --
# Expects: -DSRC_DIR=... -DDST_DIR=...
if(CMAKE_SCRIPT_MODE_FILE)
    # Copy all top-level files.
    file(GLOB _top_files "${SRC_DIR}/*")
    foreach(_f IN LISTS _top_files)
        if(NOT IS_DIRECTORY "${_f}")
            cmake_path(GET _f FILENAME _fname)
            file(COPY "${_f}" DESTINATION "${DST_DIR}")
        endif()
    endforeach()

    # Copy subdirectories, skipping _-prefixed ones.
    file(GLOB _subdirs "${SRC_DIR}/*")
    foreach(_d IN LISTS _subdirs)
        if(IS_DIRECTORY "${_d}")
            cmake_path(GET _d FILENAME _dirname)
            string(SUBSTRING "${_dirname}" 0 1 _first_char)
            if(_first_char STREQUAL "_")
                message(STATUS "deployData: skipping '${_dirname}' (_-prefixed)")
            else()
                file(COPY "${_d}" DESTINATION "${DST_DIR}")
            endif()
        endif()
    endforeach()
    return()
endif()

# -- Configuration-time function (called from CMakeLists.txt) --
function(deployData)
    cmake_parse_arguments(DD "" "TARGET;DATA_DIR;MOD_NAME" "" ${ARGN})

    if(NOT DD_TARGET OR NOT DD_DATA_DIR OR NOT DD_MOD_NAME)
        message(FATAL_ERROR "deployData: TARGET, DATA_DIR, and MOD_NAME are required.")
    endif()

    if(NOT DEFINED ENV{SKYRIM_MODS_FOLDER} OR NOT IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
        return()
    endif()

    set(_src "${CMAKE_SOURCE_DIR}/Data/${DD_DATA_DIR}")
    set(_dst "$ENV{SKYRIM_MODS_FOLDER}/${DD_MOD_NAME}")

    if(NOT IS_DIRECTORY "${_src}")
        message(WARNING "deployData: source directory '${_src}' does not exist.")
        return()
    endif()

    add_custom_command(TARGET ${DD_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR=${_src}
            -DDST_DIR=${_dst}
            -P ${CMAKE_SOURCE_DIR}/cmake/lib/deployData.cmake
        COMMENT "Deploying Data/${DD_DATA_DIR} -> ${_dst}"
        VERBATIM
    )
endfunction()
