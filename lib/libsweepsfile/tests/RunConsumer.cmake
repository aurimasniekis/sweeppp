# Configures, builds and runs a consumer of the *installed* package.
#
# Driven from ctest rather than from a Makefile somewhere above this directory,
# so that "it installs, and something can link it" is checked by the same
# command that runs every other test -- in any build tree, with no orchestration
# outside this library.
#
# `cmake -P` rather than a shell script because this has to run wherever CMake
# does, and the one thing guaranteed present on a machine building this library
# is CMake.
#
# Expects: SOURCE_DIR BINARY_DIR PREFIX EXECUTABLE GENERATOR BUILD_TYPE

foreach(required SOURCE_DIR BINARY_DIR PREFIX EXECUTABLE GENERATOR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "RunConsumer.cmake: ${required} was not set")
    endif()
endforeach()

# From scratch every time. A consumer that only builds because a stale cache
# still points at last week's prefix is not evidence of anything.
file(REMOVE_RECURSE "${BINARY_DIR}")

set(configure_args
    -S "${SOURCE_DIR}"
    -B "${BINARY_DIR}"
    -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${PREFIX}")
if(BUILD_TYPE)
    list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" ${configure_args} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "configuring ${SOURCE_DIR} against the installed package failed.\n"
        "This is what catches a header the install rule dropped, and a link a "
        "C-only project cannot complete.")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}" --config "${BUILD_TYPE}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "building ${SOURCE_DIR} against the installed package failed")
endif()

# Multi-config generators put it one level down; single-config generators do not.
set(candidates
    "${BINARY_DIR}/${EXECUTABLE}"
    "${BINARY_DIR}/${BUILD_TYPE}/${EXECUTABLE}"
    "${BINARY_DIR}/${EXECUTABLE}.exe"
    "${BINARY_DIR}/${BUILD_TYPE}/${EXECUTABLE}.exe")

set(program "")
foreach(candidate ${candidates})
    if(EXISTS "${candidate}")
        set(program "${candidate}")
        break()
    endif()
endforeach()

if(NOT program)
    message(FATAL_ERROR "built ${EXECUTABLE} but could not find it under ${BINARY_DIR}")
endif()

# In its own directory: the consumers write and then delete a session file, and
# two of them sharing a working directory would be racing over the name.
execute_process(COMMAND "${program}" WORKING_DIRECTORY "${BINARY_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${EXECUTABLE} ran against the installed package and failed")
endif()
