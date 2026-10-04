# A C consumer of the *installed* package.
#
# `LANGUAGES C`, with no CXX anywhere, and that is the entire test. It catches
# two things nothing in the build tree can see:
#
#   - the install rule. `FILES_MATCHING PATTERN "*.hpp"` would drop
#     `sweeps/sweeps.h` silently: every in-tree test would still pass, because
#     they compile against the source tree, and the first sign of trouble would
#     be a consumer's #include failing somewhere else entirely.
#
#   - the link. A static archive of C++ objects gives a C-only executable no
#     C++ runtime, so it fails on `operator new` and `__cxa_throw`. The package
#     config enables CXX for exactly that case; if it stops doing so, this
#     directory stops configuring.
#
# Its C++ sibling in ../consumer stays as it is: the two prove different claims,
# and one that linked C++ would prove neither.

cmake_minimum_required(VERSION 3.24)
project(sweeps_consumer_c LANGUAGES C)

find_package(sweepsfile REQUIRED)

add_executable(sweeps-consumer-c main.c)

# A live-stream reader: `sweeppp-cli record -o - | sweeps-stream-c`.
add_executable(sweeps-stream-c stream.c)

foreach(target sweeps-consumer-c sweeps-stream-c)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)

    target_link_libraries(${target} PRIVATE sweeps::sweepsfile)

    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
