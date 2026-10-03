set(SWEEPPP_SOURCES
    src/core/BlockPool.cpp
    src/core/CrashHandler.cpp
    src/core/EventBus.cpp
    src/core/Log.cpp
    src/core/Paths.cpp
    src/core/SystemInfo.cpp
    src/core/Telemetry.cpp
    src/core/Toml.cpp
    src/core/Version.cpp

    src/dsp/Convert.cpp

    src/crypto/Sha256.cpp

    src/correction/CorrectionLearner.cpp
    src/correction/Corrections.cpp

    src/fft/FftBackendManager.cpp
    src/fft/FftBenchmark.cpp
    src/fft/IFftBackend.cpp
    src/fft/Window.cpp

    src/history/EventMapping.cpp
    src/history/SessionRecorder.cpp
    src/history/SessionReplay.cpp

    src/instrument/LocalInstrument.cpp

    src/net/Socket.cpp

    src/pipeline/AsyncFrameConsumer.cpp
    src/pipeline/FrameBus.cpp
    src/pipeline/Pipeline.cpp

    src/plugin/DynamicLibrary.cpp
    src/plugin/PluginAbiSupport.cpp
    src/plugin/PluginFacets.cpp
    src/plugin/PluginHost.cpp
    src/plugin/PluginSearchPath.cpp
    src/plugin/PluginSettings.cpp

    src/remote/ClockMap.cpp
    src/remote/FrameCodec.cpp
    src/remote/Messages.cpp
    src/remote/RemoteInstrument.cpp
    src/remote/RemoteServer.cpp
    src/remote/ServerList.cpp
    src/remote/StreamIo.cpp
    src/remote/WireCodec.cpp

    src/rf/Antenna.cpp
    src/rf/AntennaAssignments.cpp
    src/rf/IRfPath.cpp
    src/rf/RfRouting.cpp

    src/sdr/ISdrDevice.cpp
    src/sdr/SampleFormat.cpp
    src/sdr/SdrParameter.cpp

    src/sweep/SweepEngine.cpp
    src/sweep/SweepPlan.cpp
    src/profile/Profile.cpp
    src/sweep/RangeHistory.cpp
    src/sweep/SweepPreset.cpp

    src/ui/ColorMap.cpp
    src/ui/Marker.cpp
    src/ui/MarkerPreset.cpp
    src/ui/PanelLayout.cpp
    src/ui/Theme.cpp
    src/ui/ToastCenter.cpp
    src/ui/TraceStore.cpp

    src/backends/sdr/iqfile/IqFileDevice.cpp
    src/backends/sdr/synthetic/SyntheticDevice.cpp
)

add_library(sweeppp ${SWEEPPP_SOURCES})
add_library(sweeppp::sweeppp ALIAS sweeppp)

target_include_directories(sweeppp
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# sweepsfile is PUBLIC: Sweep++'s own public headers include <sweeps/...> --
# core/Result.hpp re-exports the error type, and the history headers are
# `using` declarations over the library's.
# ${CMAKE_DL_LIBS} is the plugin host's: dlopen lives in libdl on older glibc,
# in libc on newer, and nowhere at all on macOS and Windows. Left to CMake
# rather than guessed.
target_link_libraries(sweeppp
    PUBLIC  sweeps::sweepsfile tomlplusplus::tomlplusplus Threads::Threads
    PRIVATE sweeppp_warnings ${CMAKE_DL_LIBS})

# dbghelp is the crash handler's, and nothing else in the tree needs it:
# turning a captured return address into a name is SymFromAddr's job, and the
# POSIX side gets the same from backtrace_symbols_fd in libc.
if(WIN32)
    target_link_libraries(sweeppp PRIVATE dbghelp)
endif()

# Winsock for the remote link, and the system generator its nonces come from.
if(WIN32)
    target_link_libraries(sweeppp PRIVATE ws2_32 bcrypt)
endif()

target_compile_definitions(sweeppp PRIVATE
    SWEEPPP_VERSION_STRING="${PROJECT_VERSION}"
    SWEEPPP_CHANNEL="${SWEEPPP_CHANNEL}"
    SWEEPPP_APP_ID="${SWEEPPP_APP_ID}"
    SWEEPPP_PRODUCT_NAME="${SWEEPPP_PRODUCT_NAME}"
    SWEEPPP_BUILD_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    SWEEPPP_BUILD_PLATFORM="${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")

# The commit, resolved on every build rather than at configure time. Public
# include directory: the generated header is included by Version.cpp, which is
# compiled here, but a consumer building against an installed tree gets the
# same view of it.
include(GitVersion)
sweeppp_add_build_info(sweeppp)

# The multiarch triplet, for the plugin search path.
#
# On Debian and its derivatives a packaged library lands in
# /usr/lib/<triplet>, not /usr/lib, so without this the host would not find a
# plugin installed by a .deb at all. Taken from the toolchain rather than
# guessed; empty on platforms that do not use it, which contributes nothing.
if(CMAKE_LIBRARY_ARCHITECTURE)
    target_compile_definitions(sweeppp PRIVATE
        SWEEPPP_LIBRARY_ARCHITECTURE="${CMAKE_LIBRARY_ARCHITECTURE}")
endif()

# No FFT and no radio library here, and that is the point of the plugins: FFTW
# is GPL-2.0-or-later, libhackrf is GPL-2.0-only and libbladeRF is LGPL. Each is
# linked by the one module that needs it, which is what makes "the application
# links nothing GPL" a fact rather than an intention -- `nm -u` on any binary
# this produces mentions none of the three.
