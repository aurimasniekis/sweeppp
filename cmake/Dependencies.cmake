# Where Sweep++'s dependencies come from.
#
# The C libraries -- FFTW, GLFW, libhackrf, libbladeRF, librtlsdr, libusb --
# come from the system, with no vendored fallback. Everything else is fetched
# and built from source, because nothing packages it: ImGui on its docking
# branch, ImPlot at a post-release commit, an icon webfont, a file dialog, the
# two Fobos SDR libraries, PocketFFT.
#
# What is fetched lands in ${CMAKE_BINARY_DIR}/_deps, which is FetchContent's own
# default: one tree per build directory, so no two presets share sources, and
# `rm -rf build` leaves nothing behind. It is a handful of small archives, so
# re-fetching after a clean costs seconds.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# The C libraries, from the system.
#
# Each is a real project with its own build system, each is packaged on every
# platform that can drive the hardware, and building them here would cost most of
# this repository's build time -- FFTW alone is ~500 files.
#
# A missing one is a hard error naming what to install, rather than a silent
# fallback to a private copy: two builds that differ in linkage, in reported
# version and in their licence obligations must not look alike.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

# pkg_check_modules, with a fresh look every time.
#
# Two caches would otherwise go stale together, and both have to be dropped.
# `__pkg_config_checked_<prefix>` gates re-running pkg-config at all. The
# absolute paths come from find_library, which FindPkgConfig calls once per -l
# and caches as `pkgcfg_lib_<prefix>_<name>` -- and find_library returns a cached
# path without checking that it still exists, so that is the entry that survives
# an upgrade and reaches the link line.
#
# Those paths are absolute and, on Homebrew, version-stamped, so upgrading a
# library leaves a build directory pointing into a prefix that is gone. The
# failure arrives from ninja, naming a third-party path and nothing else:
#
#   ninja: error: '/opt/homebrew/Cellar/fftw/3.3.10_3/lib/libfftw3f.dylib',
#   needed by 'dist/sweeppp-cli', missing and no known rule to make it
#
# Re-detecting costs one pkg-config exec per configure, and means an upgrade
# needs no more than the reconfigure every build already does.
macro(sweeppp_recheck_package prefix module)
    get_cmake_property(_sweeppp_cache_vars CACHE_VARIABLES)
    foreach(_sweeppp_var IN LISTS _sweeppp_cache_vars)
        if(_sweeppp_var MATCHES "^pkgcfg_lib_${prefix}_")
            unset(${_sweeppp_var} CACHE)
        endif()
    endforeach()
    unset(_sweeppp_cache_vars)
    unset(_sweeppp_var)

    unset(__pkg_config_checked_${prefix} CACHE)
    pkg_check_modules(${prefix} QUIET IMPORTED_TARGET ${module})
endmacro()

# Reports what to install rather than what CMake failed to find. A developer
# hitting this wants the command, not the module name.
#
# The status line is emitted here rather than by the caller: pkg_check_modules
# sets most of its results in the calling scope, so a caller outside this
# function sees only the handful it caches.
function(sweeppp_require_package module target brewName aptName)
    sweeppp_recheck_package(${target} ${module})
    if(NOT ${target}_FOUND)
        message(FATAL_ERROR
            "${module} was not found, and Sweep++ does not vendor it.\n"
            "  macOS:  brew install ${brewName}\n"
            "  Debian: sudo apt install ${aptName}\n")
    endif()

    # Homebrew's libhackrf.pc carries no Version field, so version is routinely
    # blank; the library path is what identifies the copy in that case.
    if(${target}_VERSION)
        message(STATUS "${module}: ${${target}_VERSION}")
    elseif(${target}_LINK_LIBRARIES)
        message(STATUS "${module}: ${${target}_LINK_LIBRARIES}")
    else()
        message(STATUS "${module}: found")
    endif()
endfunction()

# Pinned revisions. Bumping any of these is a deliberate act; see THIRD_PARTY.md.
set(SWEEPPP_PIN_IMGUI   "v1.92.9b-docking")
# ImPlot post-v1.0. See the ImGui/ImPlot block below for why a tag will not do.
set(SWEEPPP_PIN_IMPLOT  "7eeb9168d2e5e6b14e266d8782ecf7e649dfc3a4")
# Material Design Icons webfont. Apache-2.0, so GPL-3.0 compatible; the only
# reason it is fetched rather than committed is the vendor-everything rule.
set(SWEEPPP_PIN_MDI     "v7.4.47")
# Native file dialogs. Zlib. GLFW deliberately has none, and shelling out to
# osascript/zenity is neither native nor reliable.
set(SWEEPPP_PIN_NFD     "v1.2.1")
# stb has no releases; a commit is the only thing there is to pin.
set(SWEEPPP_PIN_STB     "2c980bb59875b0d32144a71867fbdebb2f77cd20")
set(SWEEPPP_PIN_TOMLPP  "v3.4.0")
# Monocypher, for the remote link's encryption. CC0 or BSD-2-Clause.
set(SWEEPPP_PIN_MONOCYPHER "4.0.2")
set(SWEEPPP_PIN_JSON    "v3.12.0")
set(SWEEPPP_PIN_DOCTEST "v2.4.12")
# RigExpert's Fobos SDR libraries, one per firmware. Both tags are upstream's
# own spelling, the stray dot in the agile one included.
set(SWEEPPP_PIN_LIBFOBOS       "v2.4.0")
set(SWEEPPP_PIN_LIBFOBOS_AGILE "v.3.3.0")
# PocketFFT's C++ header, from its `cpp` branch. No releases; the commit is the
# only pin, as with stb.
set(SWEEPPP_PIN_POCKETFFT "c90e55b3d529f8efa40ed01a20de22405f45fc65")

# Vendored projects are third-party code; our warning set would drown the build.
# Saved/restored around each block that needs it.
set(_sweeppp_saved_c_flags   "${CMAKE_C_FLAGS}")
set(_sweeppp_saved_cxx_flags "${CMAKE_CXX_FLAGS}")

macro(sweeppp_quiet_vendor)
    if(NOT MSVC)
        set(CMAKE_C_FLAGS   "${_sweeppp_saved_c_flags} -w")
        set(CMAKE_CXX_FLAGS "${_sweeppp_saved_cxx_flags} -w")
    endif()
endmacro()

macro(sweeppp_restore_flags)
    set(CMAKE_C_FLAGS   "${_sweeppp_saved_c_flags}")
    set(CMAKE_CXX_FLAGS "${_sweeppp_saved_cxx_flags}")
endmacro()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# toml++ -- every on-disk config file: profiles, presets, sweep plans,
# band plans, themes. Header-only.
# ---------------------------------------------------------------------------
FetchContent_Declare(tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/${SWEEPPP_PIN_TOMLPP}.tar.gz)
FetchContent_MakeAvailable(tomlplusplus)

# ---------------------------------------------------------------------------
# Monocypher -- X25519, ChaCha20-Poly1305 and BLAKE2b for the remote link's
# Noise handshake. One C file and one header, audited, with no build system
# worth adding: the archive is populated and the file compiled here.
# ---------------------------------------------------------------------------
FetchContent_Declare(monocypher
    URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/${SWEEPPP_PIN_MONOCYPHER}.tar.gz)
FetchContent_MakeAvailable(monocypher)

add_library(sweeppp_monocypher STATIC "${monocypher_SOURCE_DIR}/src/monocypher.c")
target_include_directories(sweeppp_monocypher SYSTEM PUBLIC "${monocypher_SOURCE_DIR}/src")
set_target_properties(sweeppp_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(sweeppp_monocypher PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# doctest
# ---------------------------------------------------------------------------
if(SWEEPPP_BUILD_TESTS)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/${SWEEPPP_PIN_DOCTEST}.tar.gz)
    FetchContent_MakeAvailable(doctest)

    # doctest is third-party, so its headers are third-party: a diagnostic
    # whose location is inside doctest.h is not one we can act on, and the
    # sanitiser jobs build with -Werror. Expressed through the include path
    # because doctest is header-only -- the same treatment the vendored ImGui
    # gets from target_include_directories(... SYSTEM ...).
    get_target_property(_doctest_includes doctest INTERFACE_INCLUDE_DIRECTORIES)
    if(_doctest_includes)
        set_target_properties(doctest PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_doctest_includes}")
    endif()

    # doctest forward-declares std::basic_ostream instead of including
    # <ostream>, and turns that off by itself only for libc++. MSVC's
    # <string_view> defines operator<<(ostream&, string_view) inline and needs
    # the complete type, so stringifying a string_view in any CHECK fails
    # inside MSVC's own headers. Set everywhere rather than only for MSVC: one
    # behaviour for every compiler is worth more than the includes it saves,
    # and libc++ is already on this path.
    target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

endif()

# ---------------------------------------------------------------------------
# nlohmann/json -- scoped to bin/sweeppp-server's HTTP/WS wire format only.
# Deliberately not used for any config file; that is toml++'s job.
# ---------------------------------------------------------------------------
if(SWEEPPP_BUILD_SERVER OR SWEEPPP_WITH_UPDATE_CHECK)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install    OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/archive/refs/tags/${SWEEPPP_PIN_JSON}.tar.gz)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# libcurl -- the update check, and nothing else.
#
# One transport rather than three: WinHTTP, NSURLSession and something on
# Linux would be three implementations of one GET, each with its own TLS
# behaviour and its own way of being wrong behind a proxy. curl is on the
# macOS SDK, packaged everywhere on Linux and available from vcpkg.
#
# A build without it degrades to no update check rather than failing, which is
# what makes SWEEPPP_WITH_UPDATE_CHECK an option and not a requirement.
# ---------------------------------------------------------------------------
if(SWEEPPP_WITH_UPDATE_CHECK)
    find_package(CURL QUIET)
    if(NOT CURL_FOUND)
        message(WARNING
            "libcurl not found; building without the update check. "
            "Install libcurl4-openssl-dev (Debian/Ubuntu) or curl (vcpkg) to enable it.")
        set(SWEEPPP_WITH_UPDATE_CHECK OFF CACHE BOOL "Ask GitHub about newer releases" FORCE)
    endif()
endif()

# ---------------------------------------------------------------------------
# FFTW, libhackrf, libbladeRF and librtlsdr -- from the system, and only for
# their plugins.
#
# Gated on SWEEPPP_BUILD_PLUGINS as well as the per-library option, because
# nothing else links them: -DSWEEPPP_BUILD_PLUGINS=OFF must not demand that
# FFTW or libhackrf be installed to build an application that cannot reach
# either.
#
# libusb is linked for the Fobos plugin alone, because the Fobos libraries are
# compiled here and call it directly. No code of ours calls it: libhackrf does
# not expose its libusb handle, so even a link-speed readout could not reach
# it, and the other radio libraries bring their own.
# ---------------------------------------------------------------------------
if(SWEEPPP_BUILD_PLUGINS AND SWEEPPP_WITH_FFTW)
    # Single precision. The version is reported by `sweeppp-cli info` and the
    # FFT panel, so it is taken from the copy actually linked rather than from
    # a constant here.
    sweeppp_require_package(fftw3f FFTW3F fftw libfftw3-dev)

    if(FFTW3F_VERSION)
        set(SWEEPPP_FFTW_VERSION_STRING "${FFTW3F_VERSION}")
    else()
        set(SWEEPPP_FFTW_VERSION_STRING "unknown")
    endif()

    add_library(sweeppp_fftw INTERFACE)
    target_link_libraries(sweeppp_fftw INTERFACE PkgConfig::FFTW3F)

    # Threaded planning is optional: FFTW may be built with threads combined
    # into the main library, as a separate libfftw3f_threads, or not at all.
    # Only the separate form needs naming, and only if it exists.
    #
    # FFTW ships no .pc for the threads library even where it ships one for
    # fftw3f, so find_library is the path that usually answers. Its result is
    # cached too, and a cached path that no longer exists is cleared rather
    # than linked against.
    sweeppp_recheck_package(FFTW3F_THREADS fftw3f_threads)
    if(FFTW3F_THREADS_FOUND)
        target_link_libraries(sweeppp_fftw INTERFACE PkgConfig::FFTW3F_THREADS)
    else()
        if(FFTW3F_THREADS_LIBRARY AND NOT EXISTS "${FFTW3F_THREADS_LIBRARY}")
            unset(FFTW3F_THREADS_LIBRARY CACHE)
        endif()
        find_library(FFTW3F_THREADS_LIBRARY NAMES fftw3f_threads)
        if(FFTW3F_THREADS_LIBRARY)
            target_link_libraries(sweeppp_fftw INTERFACE ${FFTW3F_THREADS_LIBRARY})
        endif()
    endif()
endif()

if(SWEEPPP_BUILD_PLUGINS AND SWEEPPP_WITH_HACKRF)
    sweeppp_require_package(libhackrf HACKRF hackrf libhackrf-dev)
    add_library(sweeppp_hackrf INTERFACE)
    target_link_libraries(sweeppp_hackrf INTERFACE PkgConfig::HACKRF)
endif()

if(SWEEPPP_BUILD_PLUGINS AND SWEEPPP_WITH_BLADERF)
    sweeppp_require_package(libbladeRF BLADERF libbladerf libbladerf-dev)
    add_library(sweeppp_bladerf INTERFACE)
    target_link_libraries(sweeppp_bladerf INTERFACE PkgConfig::BLADERF)
endif()

if(SWEEPPP_BUILD_PLUGINS AND SWEEPPP_WITH_RTLSDR)
    sweeppp_require_package(librtlsdr RTLSDR librtlsdr librtlsdr-dev)
    add_library(sweeppp_rtlsdr INTERFACE)
    target_link_libraries(sweeppp_rtlsdr INTERFACE PkgConfig::RTLSDR)
endif()

# ---------------------------------------------------------------------------
# libfobos and libfobos-sdr-agile -- fetched, and compiled into the Fobos
# plugin.
#
# Vendored because nothing packages either. Both, because a Fobos SDR runs one
# of two firmwares and each library opens only its own: the standard one
# matches USB bcdDevice 0x0000 and the agile one 0x0101. With both linked, a
# radio is found by whichever library speaks its firmware.
#
# Upstream's CMake is bypassed. It builds a shared library, adds an install
# rule with /etc/udev/rules.d as an absolute destination -- on macOS too, in
# the standard library's case -- and finds libusb its own way. SOURCE_SUBDIR
# names a directory with no CMakeLists.txt in it, so MakeAvailable populates
# the sources and adds nothing.
# ---------------------------------------------------------------------------
if(SWEEPPP_BUILD_PLUGINS AND SWEEPPP_WITH_FOBOS)
    sweeppp_require_package(libusb-1.0 LIBUSB libusb libusb-1.0-0-dev)

    FetchContent_Declare(libfobos
        URL https://github.com/rigexpert/libfobos/archive/refs/tags/${SWEEPPP_PIN_LIBFOBOS}.tar.gz
        SOURCE_SUBDIR fobos)
    FetchContent_Declare(libfobos_agile
        URL https://github.com/rigexpert/libfobos-sdr-agile/archive/refs/tags/${SWEEPPP_PIN_LIBFOBOS_AGILE}.tar.gz
        SOURCE_SUBDIR fobos)
    FetchContent_MakeAvailable(libfobos libfobos_agile)

    add_library(sweeppp_vendor_fobos STATIC
        "${libfobos_SOURCE_DIR}/fobos/fobos.c"
        "${libfobos_agile_SOURCE_DIR}/fobos/fobos_sdr.c")

    # Hidden, or every function in both libraries would be exported from the
    # plugin that links them.
    set_target_properties(sweeppp_vendor_fobos PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden)

    # The two sources share exactly two external names, both debugging helpers
    # neither library calls in a normal build: print_buff and to_bin. Renamed
    # in the agile unit alone, so the standard one keeps upstream's spelling.
    set_source_files_properties("${libfobos_agile_SOURCE_DIR}/fobos/fobos_sdr.c" PROPERTIES
        COMPILE_DEFINITIONS "print_buff=fobos_sdr_print_buff;to_bin=fobos_sdr_to_bin")

    # SYSTEM, so a diagnostic inside either header is not ours to act on.
    target_include_directories(sweeppp_vendor_fobos SYSTEM PUBLIC
        "${libfobos_SOURCE_DIR}/fobos"
        "${libfobos_agile_SOURCE_DIR}/fobos")

    # The standard library includes <libusb-1.0/libusb.h> and the agile one
    # <libusb.h>. pkg-config names the directory that satisfies the second;
    # its parent satisfies the first. On Linux /usr/include already is that
    # parent, but Homebrew's and conda's prefixes are not on the default path.
    get_target_property(_libusb_includes PkgConfig::LIBUSB INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_dir IN LISTS _libusb_includes)
        string(REGEX REPLACE "/+$" "" _dir "${_dir}")
        if(_dir MATCHES "libusb-1\\.0$")
            get_filename_component(_parent "${_dir}" DIRECTORY)
            target_include_directories(sweeppp_vendor_fobos SYSTEM PRIVATE "${_parent}")
        endif()
    endforeach()

    target_link_libraries(sweeppp_vendor_fobos PUBLIC PkgConfig::LIBUSB Threads::Threads)

    if(MSVC)
        target_compile_options(sweeppp_vendor_fobos PRIVATE /w)

        # Both sources carry `#pragma comment(lib, "libusb-1.0.lib")`, which
        # the linker resolves by searching its library path -- and CMake
        # links by absolute path, so nothing puts libusb's directory there.
        get_target_property(_libusb_libraries PkgConfig::LIBUSB INTERFACE_LINK_LIBRARIES)
        foreach(_library IN LISTS _libusb_libraries)
            if(EXISTS "${_library}")
                get_filename_component(_library_dir "${_library}" DIRECTORY)
                target_link_directories(sweeppp_vendor_fobos PUBLIC "${_library_dir}")
            endif()
        endforeach()
    else()
        target_compile_options(sweeppp_vendor_fobos PRIVATE -w)
        target_link_libraries(sweeppp_vendor_fobos PUBLIC m)
    endif()
endif()

# ---------------------------------------------------------------------------
# PocketFFT -- fetched, header-only, for its plugin alone.
#
# BSD-3-Clause, so the transform it gives Linux and Windows carries no GPL.
# Like stb, the archive is one header and an include directory is the whole
# integration.
# ---------------------------------------------------------------------------
if(SWEEPPP_BUILD_PLUGINS AND SWEEPPP_WITH_POCKETFFT)
    FetchContent_Declare(pocketfft
        URL https://github.com/mreineck/pocketfft/archive/${SWEEPPP_PIN_POCKETFFT}.tar.gz)
    FetchContent_MakeAvailable(pocketfft)

    add_library(sweeppp_pocketfft INTERFACE)
    target_include_directories(sweeppp_pocketfft SYSTEM INTERFACE "${pocketfft_SOURCE_DIR}")

    # NO_MULTITHREADING: the pipeline already runs one plan on N workers, and a
    # thread pool inside each transform would only fight them for cores.
    #
    # USE_POSIX_MEMALIGN: the header's portable aligned allocator keeps its
    # bookkeeping at ptr[-1], which upstream reports trips ASan's heap redzone.
    # posix_memalign has no such trick; the header ignores the macro on Windows.
    #
    # POCKETFFT_CACHE_SIZE stays at its default of 0. A non-zero cache is a
    # process-wide plan table, and without it planning shares no state at all.
    target_compile_definitions(sweeppp_pocketfft INTERFACE
        POCKETFFT_NO_MULTITHREADING
        POCKETFFT_USE_POSIX_MEMALIGN)
endif()

# ---------------------------------------------------------------------------
# GLFW -- from the system, and only when the GUI is built.
#
# It ships a CMake package, so this is find_package rather than pkg-config; the
# imported `glfw` target is what the ImGui backend links.
# ---------------------------------------------------------------------------
if(SWEEPPP_BUILD_GUI)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/gl" "${CMAKE_BINARY_DIR}/third_party/gl")

    find_package(glfw3 3.3 QUIET)
    if(NOT glfw3_FOUND)
        message(FATAL_ERROR
            "GLFW was not found, and Sweep++ does not vendor it.\n"
            "  macOS:  brew install glfw\n"
            "  Debian: sudo apt install libglfw3-dev\n"
            "Or configure with -DSWEEPPP_BUILD_GUI=OFF to skip the desktop UI.\n")
    endif()
    message(STATUS "glfw3: ${glfw3_VERSION}")

    # -----------------------------------------------------------------------
    # Dear ImGui (docking branch) + ImPlot. Neither ships a CMakeLists, so we
    # compile both into one target.
    #
    # This pairing is the project's one genuine dependency risk, and it needed
    # resolving before any UI code was written. Findings:
    #
    #   * The ImTextureID -> ImTextureRef change is a non-issue. ImPlot v1.0
    #     already declares PlotImage() taking ImTextureRef natively.
    #   * The break that does bite is ImDrawList::AddPolyline(), whose last two
    #     parameters swapped in ImGui 19276. ImGui marks the old order
    #     `= delete`, so it is a hard compile error, not a silent misbehaviour.
    #     ImPlot v1.0 predates the swap and does not build against 1.92.9b.
    #
    # ImPlot master carries the `#if IMGUI_VERSION_NUM < 19276` guard for it
    # (plus a 19299 guard for _SelectLineTexture, which correctly stays off at
    # 19291), so the pin is a post-v1.0 commit rather than the v1.0 tag.
    # Verified building clean against v1.92.9b-docking = IMGUI_VERSION_NUM 19291.
    #
    # Re-check both guards whenever either pin moves.
    # -----------------------------------------------------------------------
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/${SWEEPPP_PIN_IMGUI}.tar.gz)
    FetchContent_MakeAvailable(imgui)

    # The icon font.
    #
    # Fetched as a repository rather than a bare file so the version is pinned
    # the same way every other dependency is. Only the .ttf is used; nothing in
    # it is built.
    FetchContent_Declare(mdi_font
        URL https://github.com/Templarian/MaterialDesign-Webfont/archive/refs/tags/${SWEEPPP_PIN_MDI}.tar.gz)
    FetchContent_MakeAvailable(mdi_font)

    set(SWEEPPP_ICON_FONT "${mdi_font_SOURCE_DIR}/fonts/materialdesignicons-webfont.ttf"
        CACHE INTERNAL "Path to the vendored icon font")
    if(NOT EXISTS "${SWEEPPP_ICON_FONT}")
        message(WARNING
            "Icon font not found at ${SWEEPPP_ICON_FONT}; the UI falls back to text labels.")
        set(SWEEPPP_ICON_FONT "" CACHE INTERNAL "Path to the vendored icon font" FORCE)
    endif()

    # Native save/open dialogs: NSSavePanel on macOS, IFileDialog on Windows,
    # the portal or GTK on Linux. Typing a path into a text field is not an
    # acceptable substitute for the one interaction every operator already
    # knows.
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nfd
        URL https://github.com/btzy/nativefiledialog-extended/archive/refs/tags/${SWEEPPP_PIN_NFD}.tar.gz)
    FetchContent_MakeAvailable(nfd)

    # stb_image_write, for the snapshot button. Public domain, header-only,
    # and one translation unit in this tree defines STB_IMAGE_WRITE_IMPLEMENTATION.
    #
    # Fetched rather than vendored so it is pinned the way everything else is,
    # and declared without MakeAvailable: the archive has no CMakeLists, so
    # there is nothing to add -- an include directory is the whole integration.
    FetchContent_Declare(stb
        URL https://github.com/nothings/stb/archive/${SWEEPPP_PIN_STB}.tar.gz)
    FetchContent_MakeAvailable(stb)

    add_library(sweeppp_stb INTERFACE)
    target_include_directories(sweeppp_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

    # Not GIT_SHALLOW: the pin is a raw commit, not a tag.
    FetchContent_Declare(implot
        URL https://github.com/epezent/implot/archive/${SWEEPPP_PIN_IMPLOT}.tar.gz)
    FetchContent_MakeAvailable(implot)

    add_library(sweeppp_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
        "${implot_SOURCE_DIR}/implot.cpp"
        "${implot_SOURCE_DIR}/implot_items.cpp"
        "${implot_SOURCE_DIR}/implot_demo.cpp")

    target_include_directories(sweeppp_imgui SYSTEM PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
        "${implot_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/third_party/imgui_config")

    # A megabin sweep produces far more than 65k vertices per draw list, so
    # 32-bit indices are mandatory, not a nicety. Enforced through our own
    # imconfig header (IMGUI_USER_CONFIG) rather than by patching imgui.
    #
    # imgui_impl_opengl3 keeps its own bundled loader; our renderer uses the
    # separate sweeppp_gl loader. The two never share a translation unit.
    target_compile_definitions(sweeppp_imgui PUBLIC
        IMGUI_USER_CONFIG="sweeppp_imconfig.h"
        GLFW_INCLUDE_NONE)

    target_link_libraries(sweeppp_imgui PUBLIC glfw)

    if(NOT MSVC)
        target_compile_options(sweeppp_imgui PRIVATE -w)
    endif()
endif()
