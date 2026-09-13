sweeppp_add_plugin(sweeppp_plugin_sdr_rtlsdr
    NAME sdr-rtlsdr
    SOURCES
        src/RtlSdrDevice.cpp
        src/RtlSdrPlugin.cpp
    # No UI: this driver draws nothing. The SDR panel is generated from the
    # device's own parameters(), which is what lets a driver ship without a
    # line of interface code.
    #
    # libsweeppp is for VALUES only -- SdrParameter, SampleFormat, Result. The
    # rule the plugin must not break is at the top of
    # <sweeppp/plugin/Plugin.hpp>: link libsweeppp for values, never touch a
    # singleton. SdrDeviceManager::instance() here would be a private registry
    # the host never sees.
    LINK sweeppp_rtlsdr sweeppp::sweeppp)

# rtlsdr_set_tuner_bandwidth() and rtlsdr_set_bias_tee() are newer than some
# librtlsdr copies still packaged. Probed rather than version-tested because
# rtl-sdr.h carries no version macro to test, and a missing one only removes
# the parameter that needs it.
include(CheckCXXSourceCompiles)
get_target_property(_rtlsdr_includes PkgConfig::RTLSDR INTERFACE_INCLUDE_DIRECTORIES)
set(CMAKE_REQUIRED_INCLUDES ${_rtlsdr_includes})
# Named in unevaluated operands rather than called, for the reason the HackRF
# probe gives: the imported target carrying the library does not exist inside
# a try_compile project, so a check that links would always fail.
check_cxx_source_compiles("
    #include <rtl-sdr.h>
    int main() { return static_cast<int>(sizeof(&rtlsdr_set_bias_tee)); }"
    SWEEPPP_RTLSDR_HAS_BIAS_TEE)
check_cxx_source_compiles("
    #include <rtl-sdr.h>
    int main() { return static_cast<int>(sizeof(&rtlsdr_set_tuner_bandwidth)); }"
    SWEEPPP_RTLSDR_HAS_BANDWIDTH)
unset(CMAKE_REQUIRED_INCLUDES)

foreach(_probe SWEEPPP_RTLSDR_HAS_BIAS_TEE SWEEPPP_RTLSDR_HAS_BANDWIDTH)
    if(${_probe})
        target_compile_definitions(sweeppp_plugin_sdr_rtlsdr PRIVATE ${_probe}=1)
    else()
        target_compile_definitions(sweeppp_plugin_sdr_rtlsdr PRIVATE ${_probe}=0)
    endif()
endforeach()
if(NOT SWEEPPP_RTLSDR_HAS_BIAS_TEE)
    message(STATUS "librtlsdr: no bias-tee API; RTL-SDR devices offer no bias tee")
endif()
if(NOT SWEEPPP_RTLSDR_HAS_BANDWIDTH)
    message(STATUS "librtlsdr: no tuner-bandwidth API; RTL-SDR devices offer no filter control")
endif()

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
