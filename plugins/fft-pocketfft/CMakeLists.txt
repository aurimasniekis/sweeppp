# The PocketFFT backend.
#
# `sweeppp_pocketfft` is the fetched header and its compile definitions, from
# cmake/Dependencies.cmake. Nothing is linked in the binary sense: the whole
# transform is templates instantiated in this module.
sweeppp_add_plugin(sweeppp_plugin_fft_pocketfft
    NAME fft-pocketfft
    SOURCES
        src/PocketFftBackend.cpp
        src/PocketFftPlugin.cpp
    # No UI: the FFT panel is the host's, generated from the backend's own
    # capabilities().
    #
    # libsweeppp is for VALUES only -- IFftBackend, IFftPlan, Result. See the
    # rule at the top of <sweeppp/plugin/Plugin.hpp>.
    LINK sweeppp_pocketfft sweeppp::sweeppp)

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
