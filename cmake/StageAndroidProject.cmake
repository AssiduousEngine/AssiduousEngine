# SPDX-License-Identifier: MIT
# Copyright (c) 2026-Present Robert Campbell

# StageAndroidProject.cmake - run as `cmake -P` after the Android player links: puts the app's
# Gradle project beside the player library, so the Bin directory holds everything an Android
# export template carries, and lists it in the player's runtime-libs for the template to copy.
#
#   -DPROJECT_DIR=<the Gradle project: Code/Engine/Engine.Player/Android>
#   -DSDL_JAVA_DIR=<SDL's Java glue: ThirdParty/SDL3/android-project/app/src/main/java>
#   -DNOTICES=<THIRD-PARTY-NOTICES.txt>
#   -DOUT_DIR=<the player's directory>
#   -DPLAYER=<the player's base name>
#
# The project goes to <OUT_DIR>/android with SDL's glue in its sdl-java/ (where the app's
# build.gradle looks first), leaving out Gradle's own state. <PLAYER>.runtime-libs lists the
# notices and every file staged, relative to OUT_DIR.

foreach(_var PROJECT_DIR SDL_JAVA_DIR NOTICES OUT_DIR PLAYER)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "StageAndroidProject: ${_var} is not set")
    endif()
endforeach()

set(_dest "${OUT_DIR}/android")
file(REMOVE_RECURSE "${_dest}")
set(_listed "THIRD-PARTY-NOTICES.txt")
file(COPY "${NOTICES}" DESTINATION "${OUT_DIR}")

# Copies each file under <from> to <to>, keeping its permissions (gradlew runs), and lists it.
function(_stage_tree from to)
    file(GLOB_RECURSE _files RELATIVE "${from}" "${from}/*")
    foreach(_file IN LISTS _files)
        if(_file MATCHES "^\\.gradle/" OR _file MATCHES "(^|/)build/" OR _file STREQUAL "local.properties")
            continue()
        endif()
        get_filename_component(_dir "${to}/${_file}" DIRECTORY)
        file(COPY "${from}/${_file}" DESTINATION "${_dir}")
        file(RELATIVE_PATH _rel "${OUT_DIR}" "${to}/${_file}")
        list(APPEND _listed "${_rel}")
    endforeach()
    set(_listed "${_listed}" PARENT_SCOPE)
endfunction()

_stage_tree("${PROJECT_DIR}" "${_dest}")
_stage_tree("${SDL_JAVA_DIR}" "${_dest}/sdl-java")

list(JOIN _listed "\n" _content)
file(WRITE "${OUT_DIR}/${PLAYER}.runtime-libs" "${_content}\n")
