# ---------------------------------------------------------------------------
set(BEHAVIOR_TARGET "EngineRelayBehaviors")
set(MOD_FOLDER_NAME "Engine Relay")

# ============================================================================
# Creature-type loop
#
# Each entry in _bsb_creatures is a sub-directory under
# behavior_src/Engine Relay/ that contains a behaviors/ folder.
# The output deploys to:
#   meshes/actors/<creature>/behaviors/<behavior>.hkx
#
# To add a new creature type, append its directory name to this list.
# ============================================================================
set(_bsb_creatures character dragon)

# Parallel lists that accumulate one entry per compiled behavior.
set(_all_hkx_outputs "")   # full path in binary dir (creature-prefixed)
set(_all_hkx_targets "")   # bare deploy filename   (e.g. 0_master.hkx)
set(_all_hkx_actors  "")   # Skyrim actors sub-path (e.g. character, dragon)

foreach(_creature IN LISTS _bsb_creatures)
    set(_behaviors_dir "${CMAKE_CURRENT_SOURCE_DIR}/${_creature}/behaviors")
    if(NOT IS_DIRECTORY "${_behaviors_dir}")
        continue()
    endif()

    # Discover every *.hkx folder for this creature type.
    file(GLOB _hkx_dirs LIST_DIRECTORIES true "${_behaviors_dir}/*.hkx")

    foreach(_src_dir IN LISTS _hkx_dirs)
        if(NOT IS_DIRECTORY "${_src_dir}")
            continue()
        endif()
        if(NOT EXISTS "${_src_dir}/behavior.yaml")
            continue()
        endif()

        # Derive the behavior name from the folder name (strip the .hkx suffix).
        get_filename_component(_folder_name "${_src_dir}" NAME)
        string(REGEX REPLACE "\\.hkx$" "" _behavior_name "${_folder_name}")

        # Prefix with creature type in the build dir to avoid name collisions
        # between creature types that happen to share a behavior file name.
        set(_xml "${CMAKE_CURRENT_BINARY_DIR}/${_creature}__${_behavior_name}.xml")
        set(_hkx "${CMAKE_CURRENT_BINARY_DIR}/${_creature}__${_behavior_name}.hkx")

        # Step 1: YAML source tree -> packfile XML
        add_custom_command(
            OUTPUT  "${_xml}"
            COMMAND "${HKBUILD_EXE}"
                    "${_src_dir}" -o "${_xml}"
            DEPENDS "${_src_dir}/behavior.yaml" HKBuild
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "HKBuild: compiling ${_behavior_name} [${_creature}] (YAML -> XML)"
            VERBATIM
        )

        # Step 2: packfile XML -> binary HKX
        add_custom_command(
            OUTPUT  "${_hkx}"
            COMMAND "${HKBUILD_EXE}"
                    pack "${_xml}" -o "${_hkx}"
            DEPENDS "${_xml}" HKBuild
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "HKBuild: packing ${_behavior_name} [${_creature}] (XML -> HKX)"
            VERBATIM
        )

        list(APPEND _all_hkx_outputs "${_hkx}")
        list(APPEND _all_hkx_targets "${_behavior_name}.hkx")
        list(APPEND _all_hkx_actors  "${_creature}")
    endforeach()
endforeach()

add_custom_target(${BEHAVIOR_TARGET} ALL
    DEPENDS ${_all_hkx_outputs}
)

# Deploy all compiled HKXs to MO2 mods folder if SKYRIM_MODS_FOLDER is set.
if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    list(LENGTH _all_hkx_outputs _hkx_count)
    if(_hkx_count GREATER 0)
        math(EXPR _hkx_last "${_hkx_count} - 1")
        foreach(_idx RANGE 0 ${_hkx_last})
            list(GET _all_hkx_outputs ${_idx} _hkx)
            list(GET _all_hkx_targets ${_idx} _hkx_name)
            list(GET _all_hkx_actors  ${_idx} _actor_type)

            set(_deploy_dir "$ENV{SKYRIM_MODS_FOLDER}/${MOD_FOLDER_NAME}/meshes/actors/${_actor_type}/behaviors")
            add_custom_command(TARGET ${BEHAVIOR_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_deploy_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy "${_hkx}" "${_deploy_dir}/${_hkx_name}"
                COMMENT "Deploying ${_hkx_name} [${_actor_type}] to ${_deploy_dir}"
                VERBATIM
            )
        endforeach()
    endif()
endif()
