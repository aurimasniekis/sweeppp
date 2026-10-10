cmake_minimum_required(VERSION 3.24)

# Vendored deps are fetched before project() sets compiler flags we do not want
# leaking into them; see cmake/Dependencies.cmake for the ordering rules.
# Objective-C is enabled on Apple only, and only because the native file
# dialog is a Cocoa API. Nothing of ours is written in it.
if(APPLE)
    set(SWEEPPP_PROJECT_LANGUAGES C CXX OBJC)

    # Left unset, this defaults to whatever the machine doing the building
    # happens to be running, and macOS refuses to launch a bundle whose
    # minimum is newer than itself -- so a CI build on the newest image
    # produced an application that ran nowhere else.
    #
    # 13.3 is the floor the standard library sets: libc++ marks the
    # floating-point std::to_chars as introduced there, and this tree uses it.
    # Read by project() below, so it has to be set before that. The condition
    # is emptiness rather than DEFINED, because CMake caches this as an empty
    # string on macOS: a tree configured before this existed would otherwise
    # keep its empty entry for ever. FORCE is safe behind it for the same
    # reason -- an explicit -DCMAKE_OSX_DEPLOYMENT_TARGET is never empty, so it
    # is never overwritten.
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING
            "Oldest macOS the binaries will run on" FORCE)
    endif()
else()
    set(SWEEPPP_PROJECT_LANGUAGES C CXX)
endif()

project(sweeppp
    VERSION 0.2.0
    DESCRIPTION "SDR sweeping and spectrum analysis platform"
    HOMEPAGE_URL "https://github.com/aurimasniekis/sweeppp"
    LANGUAGES ${SWEEPPP_PROJECT_LANGUAGES})

# Attribution the packaged binaries carry: the macOS bundle's Info.plist and
# the Windows version resource both name a person and where the source lives.
# ASCII "(C)" rather than the glyph -- a .rc file without a byte-order mark is
# read in the host's codepage, and the glyph comes back out of it mangled.
set(SWEEPPP_AUTHOR "Aurimas Niekis")
set(SWEEPPP_COPYRIGHT "Copyright (C) 2026 ${SWEEPPP_AUTHOR}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Everything lands in one place so the app can find plugins/ and resources/
# relative to its own binary during development.
#
# Named dist/ rather than bin/: the source tree already has bin/sweeppp-cli
# etc., and CMake mirrors those as directories inside the build tree, so a
# bin/ output directory would collide with them (EISDIR at link time).
set(SWEEPPP_OUTPUT_DIR "${CMAKE_BINARY_DIR}/dist")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SWEEPPP_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SWEEPPP_OUTPUT_DIR}")

option(SWEEPPP_BUILD_TESTS      "Build the test suite"            ON)
option(SWEEPPP_BUILD_GUI        "Build the desktop GUI"           ON)
option(SWEEPPP_BUILD_SERVER     "Build the web server skeleton"   ON)
option(SWEEPPP_BUILD_PLUGINS    "Build bundled plugins"           ON)
option(SWEEPPP_WITH_FFTW        "Build the FFTW backend plugin"   ON)
option(SWEEPPP_WITH_ACCELERATE  "Build the Accelerate/vDSP backend plugin (macOS)" ON)
option(SWEEPPP_WITH_POCKETFFT   "Build the PocketFFT backend plugin" ON)
option(SWEEPPP_WITH_HACKRF      "Build the HackRF driver plugin"  ON)
option(SWEEPPP_WITH_BLADERF     "Build the bladeRF driver plugin" ON)
option(SWEEPPP_WITH_RTLSDR      "Build the RTL-SDR driver plugin" ON)
option(SWEEPPP_WITH_FOBOS       "Build the Fobos SDR driver plugin" ON)
option(SWEEPPP_WITH_UPDATE_CHECK "Ask GitHub about newer releases"  ON)
option(SWEEPPP_WERROR           "Treat warnings as errors"        OFF)

# windows.h defines min and max as macros, which turns every std::min/std::max
# in a translation unit that reached it -- through libbladeRF and libusb, here
# -- into a syntax error. Applied globally rather than per-target because the
# header arrives transitively and the fix has to precede it either way.
#
# NOMINMAX only. WIN32_LEAN_AND_MEAN belongs here by reputation but drops
# shellapi.h and shlobj.h, which the native file dialog needs for HDROP and
# ShellExecuteW -- it bought nothing for the min/max problem and cost the GUI.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

set(SWEEPPP_SANITIZER "" CACHE STRING "Sanitizer: '', address, thread, undefined, address+undefined")

# The channel: which application this build is. A nightly has its own name,
# bundle identifier, window class, icon and configuration directory, so it
# installs and runs beside a release.
#
# Nightly by default: a tree build is not a release, and one that used the
# release's settings directory would be writing into the settings of the
# Sweep++ a person has installed. Only the release preset -- what CI builds a
# v* tag with -- says otherwise.
#
# SWEEPPP_PRODUCT_NAME is what a person reads -- the window title, the .app,
# the desktop entry. SWEEPPP_APP_ID is what a machine reads -- the config
# directory, the .deb, the paths under /usr -- and the stem every branding/
# file is named by.
set(SWEEPPP_CHANNEL "nightly" CACHE STRING "Build channel: release or nightly")
set_property(CACHE SWEEPPP_CHANNEL PROPERTY STRINGS release nightly)
if(SWEEPPP_CHANNEL STREQUAL "release")
    set(SWEEPPP_APP_ID "sweeppp")
    set(SWEEPPP_PRODUCT_NAME "Sweep++")
    set(SWEEPPP_BUNDLE_ID "org.sweeppp.app")
    set(SWEEPPP_PROGID "Sweeppp.Session")
elseif(SWEEPPP_CHANNEL STREQUAL "nightly")
    set(SWEEPPP_APP_ID "sweeppp-nightly")
    set(SWEEPPP_PRODUCT_NAME "Sweep++ Nightly")
    set(SWEEPPP_BUNDLE_ID "org.sweeppp.app.nightly")
    set(SWEEPPP_PROGID "Sweeppp.Nightly.Session")
else()
    message(FATAL_ERROR "SWEEPPP_CHANNEL must be release or nightly, not '${SWEEPPP_CHANNEL}'")
endif()

include(Warnings)
include(Sanitizers)
include(Dependencies)
include(SweepppPlugin)

# libsweepsfile first: libsweeppp links it PUBLIC, and Sweep++'s public headers
# include <sweeps/...>.
#
# Its conformance suite is built here too, and deliberately so: the sanitiser
# presets run whatever this build produces, and the format tests are exactly the
# ones that want ASan -- one of them exists to catch a stale buffer sized for a
# previous frequency grid. Building the library without them would sanitise the
# application and leave the byte-handling untested.
#
# Install rules stay off. A GPL application has no business installing an MIT
# library's CMake package into the same prefix; `make sweepsfile` configures the
# directory standalone and exercises that path on its own.
set(SWEEPSFILE_BUILD_TESTS ${SWEEPPP_BUILD_TESTS} CACHE BOOL "" FORCE)
set(SWEEPSFILE_INSTALL     OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/libsweepsfile)

add_subdirectory(lib/libsweeppp)
add_subdirectory(bin/sweeppp-cli)

if(SWEEPPP_BUILD_GUI)
    add_subdirectory(bin/sweeppp)
endif()

if(SWEEPPP_BUILD_SERVER)
    add_subdirectory(bin/sweeppp-server)
endif()

# Before the plugins, and that ordering is load-bearing: each of them adds a
# test suite of its own, and CMake silently discards an `add_test` issued
# before `enable_testing()`. The suites still built, so the omission looked
# like nothing at all -- they simply never ran.
if(SWEEPPP_BUILD_TESTS)
    enable_testing()
endif()

if(SWEEPPP_BUILD_PLUGINS)
    add_subdirectory(plugins/bandplan)
    add_subdirectory(plugins/channels)
    add_subdirectory(plugins/detections)

    # The transform and the radios. `SWEEPPP_WITH_*` means "build this
    # plugin": none is compiled into libsweeppp, so turning one off removes a
    # module rather than a code path.
    #
    # Without an FFT plugin there is no transform at all, and the GUI refuses
    # to start. That is the trade the licence split buys, and it is the reason
    # every FFT option defaults ON.
    if(SWEEPPP_WITH_FFTW)
        add_subdirectory(plugins/fft-fftw)
    endif()
    # Accelerate is a macOS system framework, so this plugin exists only there
    # and needs nothing installed. On macOS it is what makes a build with every
    # other backend turned off still able to compute a spectrum.
    if(APPLE AND SWEEPPP_WITH_ACCELERATE)
        add_subdirectory(plugins/fft-accelerate)
    endif()
    # PocketFFT is BSD-licensed and fetched, so it builds everywhere with
    # nothing installed. Off macOS it is the transform that is not GPL.
    if(SWEEPPP_WITH_POCKETFFT)
        add_subdirectory(plugins/fft-pocketfft)
    endif()
    if(SWEEPPP_WITH_HACKRF)
        add_subdirectory(plugins/sdr-hackrf)
    endif()
    if(SWEEPPP_WITH_BLADERF)
        add_subdirectory(plugins/sdr-bladerf)
    endif()
    if(SWEEPPP_WITH_RTLSDR)
        add_subdirectory(plugins/sdr-rtlsdr)
    endif()
    if(SWEEPPP_WITH_FOBOS)
        add_subdirectory(plugins/sdr-fobos)
    endif()
endif()

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(tests/plugins)
endif()

message(STATUS "")
message(STATUS "Sweep++ ${PROJECT_VERSION}")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  gui ................ ${SWEEPPP_BUILD_GUI}")
message(STATUS "  server ............. ${SWEEPPP_BUILD_SERVER}")
message(STATUS "  plugins ............ ${SWEEPPP_BUILD_PLUGINS}")
message(STATUS "  tests .............. ${SWEEPPP_BUILD_TESTS}")
message(STATUS "  fftw plugin ........ ${SWEEPPP_WITH_FFTW}")
# Only where it can be built, so a Linux configure does not report a plugin it
# never produces.
if(APPLE)
    message(STATUS "  accelerate plugin .. ${SWEEPPP_WITH_ACCELERATE}")
endif()
message(STATUS "  pocketfft plugin ... ${SWEEPPP_WITH_POCKETFFT}")
message(STATUS "  hackrf plugin ...... ${SWEEPPP_WITH_HACKRF}")
message(STATUS "  bladerf plugin ..... ${SWEEPPP_WITH_BLADERF}")
message(STATUS "  rtlsdr plugin ...... ${SWEEPPP_WITH_RTLSDR}")
message(STATUS "  fobos plugin ....... ${SWEEPPP_WITH_FOBOS}")
message(STATUS "  update check ....... ${SWEEPPP_WITH_UPDATE_CHECK}")
message(STATUS "  sanitizer .......... ${SWEEPPP_SANITIZER}")
message(STATUS "")
