# The commit a binary was built from, written into a generated header.
#
# `versionString()` is a compile definition taken from PROJECT_VERSION, and
# that is all a nightly used to be able to say about itself: CI puts the short
# SHA in the *archive name* by sed-ing CMakeLists.txt, so the executable inside
# a nightly reported a bare 0.1.0. A bug report against "0.1.0" names any of
# several hundred commits.
#
# Two properties this has to have, and they pull in different directions:
#
#  * It must be a *build*-time step. Resolved at configure time, committing and
#    rebuilding without reconfiguring reports the previous commit -- which is
#    worse than reporting nothing, because it looks trustworthy.
#  * It must not force a rebuild when nothing changed. The generated header is
#    written to a temporary and copied only if it differs, so an unchanged
#    commit leaves the timestamp alone and nothing downstream recompiles.
#
# A source tree with no .git -- a release tarball, a vendored copy -- gets an
# empty suffix and builds normally. Absence of a commit is not an error.

# Script mode: this is the per-build invocation, not the include.
if(CMAKE_SCRIPT_MODE_FILE)
    set(hash "")
    set(dirty "")

    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${SWEEPPP_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
            WORKING_DIRECTORY "${SWEEPPP_SOURCE_DIR}"
            OUTPUT_VARIABLE hash
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)

        # Tracked files only. Untracked ones are usually build output or
        # scratch, and marking every build dirty for those would make the flag
        # mean nothing.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD --
            WORKING_DIRECTORY "${SWEEPPP_SOURCE_DIR}"
            RESULT_VARIABLE modified
            OUTPUT_QUIET
            ERROR_QUIET)
        if(NOT modified EQUAL 0)
            set(dirty ".dirty")
        endif()
    endif()

    if(hash STREQUAL "")
        set(SWEEPPP_BUILD_SUFFIX "")
    else()
        # Spelled exactly as the CI archive spells it, so the name of the
        # download and the string in its About box are the same text.
        set(SWEEPPP_BUILD_SUFFIX "+${hash}${dirty}")
    endif()

    configure_file("${SWEEPPP_TEMPLATE}" "${SWEEPPP_OUTPUT}.tmp" @ONLY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SWEEPPP_OUTPUT}.tmp"
                            "${SWEEPPP_OUTPUT}")
    return()
endif()

# Include mode: wires the step above into a target.
#
# ALL, and a target rather than a custom command on a file: the header has no
# dependency any generator could compute -- committing changes its content
# without touching a single file it is built from -- so it has to be regarded
# as always out of date and left to copy_if_different to decide whether that
# matters.
function(sweeppp_add_build_info target)
    set(generated "${CMAKE_BINARY_DIR}/generated")
    set(output "${generated}/sweeppp/core/BuildInfo.hpp")

    add_custom_target(sweeppp_build_info ALL
        BYPRODUCTS "${output}"
        COMMAND "${CMAKE_COMMAND}"
                -DSWEEPPP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DSWEEPPP_TEMPLATE=${CMAKE_SOURCE_DIR}/cmake/BuildInfo.hpp.in
                -DSWEEPPP_OUTPUT=${output}
                -P "${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake"
        COMMENT "Resolving the build's commit"
        VERBATIM)

    add_dependencies(${target} sweeppp_build_info)
    target_include_directories(${target} PUBLIC "${generated}")

    # Written once here as well, so a configure followed by anything that reads
    # the header before the target has run -- clangd, a compile database
    # consumer -- finds a file rather than a missing include.
    execute_process(COMMAND "${CMAKE_COMMAND}"
                            -DSWEEPPP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                            -DSWEEPPP_TEMPLATE=${CMAKE_SOURCE_DIR}/cmake/BuildInfo.hpp.in
                            -DSWEEPPP_OUTPUT=${output}
                            -P "${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake")
endfunction()
