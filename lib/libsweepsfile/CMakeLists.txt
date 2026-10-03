# libsweepsfile -- the `.sweeps` session container, standalone.
#
# Dual-mode by design. Configured directly it is a top-level project that
# fetches its one dependency, builds its own tests and installs a usable CMake
# package; added with add_subdirectory() from Sweep++ it is a plain library
# target that inherits the parent's dependency cache.
#
# Both modes are exercised: CI configures this directory on its own, because
# "still builds standalone" and "still compiles as C++17" are properties that
# rot within a month if nothing checks them.

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PROJECT_NAME)
    set(SWEEPSFILE_STANDALONE TRUE)
else()
    set(SWEEPSFILE_STANDALONE FALSE)
endif()

if(SWEEPSFILE_STANDALONE)
    project(sweepsfile
        VERSION 1.0.0
        DESCRIPTION "Reader and writer for the .sweeps spectrum session container"
        HOMEPAGE_URL "https://github.com/aurimasniekis/sweeppp"
        LANGUAGES CXX)

    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
    endif()
else()
    # Version is the *format* version, not the host application's.
    set(SWEEPSFILE_VERSION 1.0.0)
endif()

if(SWEEPSFILE_STANDALONE)
    set(SWEEPSFILE_VERSION ${PROJECT_VERSION})
endif()

string(REPLACE "." ";" SWEEPSFILE_VERSION_PARTS "${SWEEPSFILE_VERSION}")
list(GET SWEEPSFILE_VERSION_PARTS 0 SWEEPSFILE_VERSION_MAJOR)
list(GET SWEEPSFILE_VERSION_PARTS 1 SWEEPSFILE_VERSION_MINOR)
list(GET SWEEPSFILE_VERSION_PARTS 2 SWEEPSFILE_VERSION_PATCH)

option(SWEEPSFILE_BUILD_TESTS "Build the libsweepsfile conformance suite"
       ${SWEEPSFILE_STANDALONE})
option(SWEEPSFILE_BUILD_CLI   "Build the `sweeps` command-line tool" ON)
option(SWEEPSFILE_INSTALL     "Generate install and export rules" ON)

# The language standard this library is compiled at.
#
# 17 by default, and that default is the product: a reader that builds into
# projects which are not on C++23, which is most of them. It is a variable
# rather than a hard-coded property so a consumer already on a later standard
# can raise it and get `std::expected` in the API instead of the vendored one --
# see below.
set(SWEEPSFILE_CXX_STANDARD 17 CACHE STRING
    "C++ standard to compile libsweepsfile at (17, 20 or 23)")
set_property(CACHE SWEEPSFILE_CXX_STANDARD PROPERTY STRINGS 17 20 23)

# ---------------------------------------------------------------------------
# expected
#
# No dependency, fetched or otherwise. `sweeps/vendor/expected.hpp` is
# tl::expected v1.1.0 (CC0-1.0) with its namespace and macros renamed into this
# library -- see the note at the top of that file for why the rename is
# load-bearing rather than cosmetic.
#
# When the standard we are compiled at provides <expected>, that is used
# instead, so a project already on C++23 gets `std::expected` in the API and
# needs no conversion at the boundary.
#
# **The choice is made here, once, and recorded in the generated Config.hpp.**
# It must not be re-decided in the header from the *consumer's* standard: the
# compiled library would return one type and the consumer would expect another
# of a different layout, which no linker would catch. Baking it into an
# installed header is what keeps the two in agreement.
# ---------------------------------------------------------------------------
include(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS "")
set(CMAKE_REQUIRED_DEFINITIONS "")
cmake_policy(SET CMP0067 NEW) # honour CMAKE_CXX_STANDARD in the check below
set(CMAKE_CXX_STANDARD ${SWEEPSFILE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)

check_cxx_source_compiles("
    #include <expected>
    int main() {
        std::expected<int, int> value{42};
        return value.has_value() ? 0 : 1;
    }" SWEEPSFILE_HAS_STD_EXPECTED)

if(SWEEPSFILE_HAS_STD_EXPECTED)
    set(SWEEPSFILE_HAS_STD_EXPECTED_01 1)
    message(STATUS "libsweepsfile: using std::expected (C++${SWEEPSFILE_CXX_STANDARD})")
else()
    set(SWEEPSFILE_HAS_STD_EXPECTED_01 0)
    message(STATUS "libsweepsfile: using the vendored expected "
                   "(C++${SWEEPSFILE_CXX_STANDARD} has no <expected>)")
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------
set(SWEEPSFILE_SOURCES
    src/AcquisitionConfig.cpp
    src/CApi.cpp
    src/Clock.cpp
    src/FileFormat.cpp
    src/Metadata.cpp
    src/Records.cpp
    src/Result.cpp
    src/SessionReader.cpp
    src/SessionWriter.cpp
    src/Stream.cpp
    src/Text.cpp
    src/WindowType.cpp
)

# Exactly one platform mapping implementation is compiled, so <windows.h>
# reaches one translation unit and no header.
if(WIN32)
    list(APPEND SWEEPSFILE_SOURCES src/MappedFileWindows.cpp)
else()
    list(APPEND SWEEPSFILE_SOURCES src/MappedFilePosix.cpp)
endif()

add_library(sweepsfile ${SWEEPSFILE_SOURCES})
add_library(sweeps::sweepsfile ALIAS sweepsfile)

# Set on the target rather than globally. Sweep++ builds at C++23, and without
# pinning it here the constraint that makes this library reusable would be a
# comment that nothing enforces.
set_target_properties(sweepsfile PROPERTIES
    CXX_STANDARD ${SWEEPSFILE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION ${SWEEPSFILE_VERSION}
    SOVERSION 1)

target_include_directories(sweepsfile
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# CXX_STANDARD above governs how *this* library is compiled; it says nothing to
# a consumer. Without this, a project that never set a standard of its own
# builds against these headers at the compiler's default and fails on
# `std::is_integral_v`. The install round-trip test is what surfaced that.
#
# It is the *minimum* a consumer must compile at, so it tracks what the headers
# actually need: raising SWEEPSFILE_CXX_STANDARD to get std::expected puts that
# type in the public API, and a consumer below C++23 could not then use it.
target_compile_features(sweepsfile PUBLIC cxx_std_${SWEEPSFILE_CXX_STANDARD})

# The C ABI's export macro, from both sides.
#
# SWEEPS_BUILDING_LIBRARY selects dllexport while compiling the library itself.
# SWEEPS_BUILD_SHARED is PUBLIC because the *consumer* cannot work out how this
# library was built and would otherwise have to guess between dllimport and
# nothing -- so the answer is recorded here and travels with the export.
#
# Deliberately no CXX_VISIBILITY_PRESET hidden on this target. It would hide the
# C++ API from a shared build, and Sweep++ links that API.
target_compile_definitions(sweepsfile PRIVATE SWEEPS_BUILDING_LIBRARY=1)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(sweepsfile PUBLIC SWEEPS_BUILD_SHARED=1)
endif()

if(NOT MSVC)
    target_compile_options(sweepsfile PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/sweeps/Config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/sweeps/Config.hpp"
    @ONLY)

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------
if(SWEEPSFILE_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS sweepsfile
        EXPORT sweepsfile-targets
        ARCHIVE  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME  DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

    # Both patterns. FILES_MATCHING drops everything that matches none of them,
    # so the C ABI's `sweeps.h` -- the only `.h` here -- would install silently
    # as nothing, and the failure would surface in a consumer rather than here.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/sweeps"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/sweeps/Config.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sweeps")

    # The specification ships with the library. A consumer who resolves this
    # package gets the document that makes the format implementable without the
    # headers -- which is the point of publishing it at all. The licence goes
    # with it because an MIT library that installs without one is not one anybody
    # can redistribute.
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/sweeps-format-v1.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")

    install(EXPORT sweepsfile-targets
        FILE sweepsfile-targets.cmake
        NAMESPACE sweeps::
        DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/sweepsfile")

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sweepsfile-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/sweepsfile-config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/sweepsfile")

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/sweepsfile-config-version.cmake"
        VERSION ${SWEEPSFILE_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/sweepsfile-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/sweepsfile-config-version.cmake"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/sweepsfile")
endif()

if(SWEEPSFILE_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(SWEEPSFILE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
