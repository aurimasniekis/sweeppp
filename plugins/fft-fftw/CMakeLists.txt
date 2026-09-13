sweeppp_add_plugin(sweeppp_plugin_fft_fftw
    NAME fft-fftw
    SOURCES
        src/FftwBackend.cpp
        src/FftwPlugin.cpp
    # No UI: the FFT panel is the host's. It is generated from the backend's
    # own capabilities(), which is what lets a backend ship without a line of
    # interface code.
    #
    # libsweeppp is for VALUES only -- IFftBackend, IFftPlan, Result. The rule
    # the plugin must not break is at the top of <sweeppp/plugin/Plugin.hpp>:
    # link libsweeppp for values, never touch a singleton.
    # FftBackendManager::instance() here would be a private registry the host
    # never sees.
    LINK sweeppp_fftw sweeppp::sweeppp)

# The FFTW pin, so the backend can report its own version without depending on
# a symbol the public header does not declare.
target_compile_definitions(sweeppp_plugin_fft_fftw PRIVATE
    SWEEPPP_FFTW_VERSION="${SWEEPPP_FFTW_VERSION_STRING}")

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
