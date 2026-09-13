# The Accelerate backend.
#
# No entry in cmake/Dependencies.cmake, deliberately: that file exists for
# libraries that can be *missing* and so need probing with a hard error naming
# the package to install. Accelerate ships with the OS and cannot be absent, so
# a probe would be ceremony with nothing behind it.
#
# `-framework Accelerate` as a bare quoted string, following third_party/gl,
# the repo's only other framework link. sweeppp_add_plugin splices LINK
# straight into target_link_libraries(... PRIVATE ...), so this needs nothing
# added to the helper.
sweeppp_add_plugin(sweeppp_plugin_fft_accelerate
    NAME fft-accelerate
    SOURCES
        src/AccelerateBackend.cpp
        src/AcceleratePlugin.cpp
    # No UI: the FFT panel is the host's, generated from the backend's own
    # capabilities().
    #
    # libsweeppp is for VALUES only -- IFftBackend, IFftPlan, Result. See the
    # rule at the top of <sweeppp/plugin/Plugin.hpp>.
    LINK "-framework Accelerate" sweeppp::sweeppp)

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
