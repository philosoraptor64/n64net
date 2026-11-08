# n64net build fingerprint.
#
# Two players stay in sync only if they run the same simulation, so the
# rendezvous server refuses to pair builds whose ids differ (the game sends
# "<game>+<build id>" as its game id; a mismatch fails with "version <host id>").
# The id hashes what decides a match: list the paths of the game's simulation
# code (and anything else that changes it, e.g. the asset extractor). Each entry
# is "<git checkout>::<path inside it>"; the id covers the committed content of
# each path plus any uncommitted edits to it (so identical edits on two
# machines still match). Leave out graphics, audio, input handling, menus art
# and platform glue: players may differ there.
#
#   include(n64net/cmake/N64NetBuildId.cmake)   # (add_subdirectory(n64net) does this)
#   n64net_add_build_id(<game target>
#       HEADER  ${CMAKE_BINARY_DIR}/generated/netplay_build_id.h
#       MACRO   MYGAME_NETPLAY_BUILD_ID
#       ENTRIES "${CMAKE_SOURCE_DIR}::src/game" "${CMAKE_SOURCE_DIR}/torch::src")
#
# The header (#define <MACRO> "<12 hex digits>") is recomputed on every build,
# since a commit changes the id without touching any file, and rewritten only
# when the value changes. Outside a git checkout the id is "nogit".

set(N64NET_BUILD_ID_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(n64net_add_build_id target)
    cmake_parse_arguments(ARG "" "HEADER;MACRO" "ENTRIES" ${ARGN})
    if (NOT ARG_HEADER OR NOT ARG_MACRO OR NOT ARG_ENTRIES)
        message(FATAL_ERROR "n64net_add_build_id: HEADER, MACRO and ENTRIES are required")
    endif()
    set(list_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_n64net_build_id_entries.txt")
    string(REPLACE ";" "\n" entries "${ARG_ENTRIES}")
    file(WRITE "${list_file}" "${entries}\n")
    add_custom_target(${target}_n64net_build_id
        COMMAND ${CMAKE_COMMAND} -DN64NET_ENTRIES_FILE=${list_file} -DN64NET_OUT=${ARG_HEADER}
                -DN64NET_MACRO=${ARG_MACRO} -P ${N64NET_BUILD_ID_SCRIPT}
        BYPRODUCTS ${ARG_HEADER}
        COMMENT "n64net: netplay build id"
        VERBATIM)
    add_dependencies(${target} ${target}_n64net_build_id)
    get_filename_component(dir "${ARG_HEADER}" DIRECTORY)
    target_include_directories(${target} PRIVATE "${dir}")
endfunction()

function(n64net_compute_build_id entries_file out_var)
    find_package(Git QUIET)
    file(STRINGS "${entries_file}" entries)
    set(parts "")
    set(index 0)
    foreach (entry ${entries})
        string(FIND "${entry}" "::" sep)
        if (sep LESS 0)
            message(FATAL_ERROR "n64net build id: bad entry '${entry}' (want <checkout>::<path>)")
        endif()
        string(SUBSTRING "${entry}" 0 ${sep} repo)
        math(EXPR start "${sep} + 2")
        string(SUBSTRING "${entry}" ${start} -1 path)
        if (NOT GIT_FOUND OR NOT EXISTS "${repo}/.git")
            set(${out_var} "nogit" PARENT_SCOPE)
            return()
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${repo}" rev-parse "HEAD:${path}"
                        OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${repo}" diff HEAD -- "${path}"
                        OUTPUT_VARIABLE diff ERROR_QUIET)
        # Labelled by position, never by location, so the id doesn't depend on
        # where (or under which folder names) each player cloned the game.
        string(APPEND parts "${index}:${path}=${id}\n${diff}")
        math(EXPR index "${index} + 1")
    endforeach()
    string(SHA1 hash "${parts}")
    string(SUBSTRING "${hash}" 0 12 short)
    set(${out_var} "${short}" PARENT_SCOPE)
endfunction()

if (CMAKE_SCRIPT_MODE_FILE AND DEFINED N64NET_ENTRIES_FILE AND DEFINED N64NET_OUT AND DEFINED N64NET_MACRO)
    n64net_compute_build_id("${N64NET_ENTRIES_FILE}" id)
    set(content "#pragma once\n#define ${N64NET_MACRO} \"${id}\"\n")
    set(old "")
    if (EXISTS "${N64NET_OUT}")
        file(READ "${N64NET_OUT}" old)
    endif()
    if (NOT old STREQUAL content)
        file(WRITE "${N64NET_OUT}" "${content}")
        message(STATUS "Netplay build id: ${id}")
    endif()
endif()
