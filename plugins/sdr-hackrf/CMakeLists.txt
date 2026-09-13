sweeppp_add_plugin(sweeppp_plugin_sdr_hackrf
    NAME sdr-hackrf
    SOURCES
        src/HackRfDevice.cpp
        src/HackRfPlugin.cpp
    # No UI: this driver draws nothing. The SDR panel is generated from the
    # device's own parameters(), which is what lets a driver ship without a
    # line of interface code.
    #
    # libsweeppp is for VALUES only -- SdrParameter, SampleFormat, Result. The
    # rule the plugin must not break is at the top of
    # <sweeppp/plugin/Plugin.hpp>: link libsweeppp for values, never touch a
    # singleton. SdrDeviceManager::instance() here would be a private registry
    # the host never sees.
    LINK sweeppp_hackrf sweeppp::sweeppp)

# The board-revision API -- hackrf_board_rev_read(), the BOARD_REV_* constants
# and BOARD_ID_UNDETECTED -- arrived in libhackrf 2022.09.1. Every distribution
# is well past that; PothosSDR, the only prebuilt source on Windows, is still
# on 2021.07.25. Probed rather than version-tested because libhackrf.h carries
# no version macro to test.
include(CheckCXXSourceCompiles)
get_target_property(_hackrf_includes PkgConfig::HACKRF INTERFACE_INCLUDE_DIRECTORIES)
set(CMAKE_REQUIRED_INCLUDES ${_hackrf_includes})
check_cxx_source_compiles("
    #include <hackrf.h>
    #include <cstdint>
    int main() {
        // Named in unevaluated operands rather than called: the question is
        // whether the header declares this API, which is exactly what decides
        // whether HackRfDevice.cpp compiles. Calling them would make the check
        // link, and the imported target carrying libhackrf does not exist
        // inside a try_compile project.
        (void)sizeof(&hackrf_board_rev_read);
        (void)sizeof(&hackrf_board_rev_name);
        (void)sizeof(hackrf_board_rev);
        return BOARD_ID_UNDETECTED + BOARD_REV_UNDETECTED + BOARD_REV_UNRECOGNIZED +
               HACKRF_BOARD_REV_GSG + BOARD_REV_HACKRF1_OLD;
    }" SWEEPPP_HACKRF_HAS_BOARD_REV)
unset(CMAKE_REQUIRED_INCLUDES)

if(SWEEPPP_HACKRF_HAS_BOARD_REV)
    target_compile_definitions(sweeppp_plugin_sdr_hackrf PRIVATE SWEEPPP_HACKRF_HAS_BOARD_REV=1)
else()
    target_compile_definitions(sweeppp_plugin_sdr_hackrf PRIVATE SWEEPPP_HACKRF_HAS_BOARD_REV=0)
    message(STATUS "libhackrf: no board-revision API; device labels omit the revision")
endif()

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
