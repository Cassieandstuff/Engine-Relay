# Copies build outputs to configured Skyrim mod directories.
# Set the following environment variables to enable automatic deployment:
#
#   SKYRIM_OWRT_FOLDER  - path to your Skyrim overwrite/output folder
#   SKYRIM_MODS_FOLDER  - path to your mod manager's mods folder
#
# Both are optional -- any that are unset or point to non-existent
# directories are silently skipped.

function(automaticGameFolderOutput)
    if(DEFINED ENV{SKYRIM_OWRT_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_OWRT_FOLDER}")
        copyOutputs("$ENV{SKYRIM_OWRT_FOLDER}/${PRODUCT_NAME}${PROJECT_SUFFIX}")
    endif()
    if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
        copyOutputs("$ENV{SKYRIM_MODS_FOLDER}/${PRODUCT_NAME}${PROJECT_SUFFIX}")
    endif()
endfunction()
