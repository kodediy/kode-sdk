# Writes kode_sdk_version.h: the SDK's version, for the boot banner.
#
# Run as `cmake -DSDK_ROOT=<dir> -DLOCK_FILE=<file> -DOUT=<header> -P version.cmake`
# by a target that every build runs, so a commit or a checkout in the SDK shows
# up without a reconfigure. The header is rewritten only when the string
# changed, so an unchanged version rebuilds nothing.
#
# The version is, in this order:
#   1. `git describe --tags --dirty --always` in SDK_ROOT, when SDK_ROOT is the
#      SDK's own work tree (a submodule's .git is a file);
#   2. the `version:` the component manager recorded for kodediy/kode in the
#      consuming project's dependencies.lock, when the SDK was fetched and has
#      no work tree;
#   3. `unknown`.

set(sdk_version "")

# SDK_ROOT is the SDK only when it holds this component; a fetched copy sits
# in the project's managed_components, where the same relative path leads to
# the project itself, which may be a git work tree of its own.
if(EXISTS "${SDK_ROOT}/.git" AND EXISTS "${SDK_ROOT}/components/kode_sys/version.cmake")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SDK_ROOT}" describe --tags --dirty --always
            OUTPUT_VARIABLE git_describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE git_status
            ERROR_QUIET)
        if(git_status EQUAL 0)
            set(sdk_version "${git_describe}")
        endif()
    endif()
endif()

# The lock lists each component as "  <name>:" with its own "    version:" at
# the next indent level; the entries nested under "dependencies:" sit deeper,
# so the first four-space "version:" after the kodediy/kode line is its own.
if(sdk_version STREQUAL "" AND EXISTS "${LOCK_FILE}")
    file(STRINGS "${LOCK_FILE}" lock_lines)
    set(in_entry FALSE)
    foreach(line IN LISTS lock_lines)
        if(line MATCHES "^  kodediy/kode:$")
            set(in_entry TRUE)
        elseif(in_entry AND line MATCHES "^  [^ ]")
            break()
        elseif(in_entry AND line MATCHES "^    version: *(.+)$")
            set(sdk_version "${CMAKE_MATCH_1}")
            break()
        endif()
    endforeach()
endif()

if(sdk_version STREQUAL "")
    set(sdk_version "unknown")
endif()

set(content
"/**
 * @file kode_sdk_version.h
 * @brief The SDK's version, written by the build from git; never edited by hand.
 */
#ifndef KODE_SDK_VERSION_H
#define KODE_SDK_VERSION_H

/** The SDK's version: its git describe string, else the one the component manager recorded, else unknown. */
#define KODE_SDK_VERSION \"${sdk_version}\"

#endif /* KODE_SDK_VERSION_H */
")

set(old_content "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old_content)
endif()
if(NOT old_content STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
