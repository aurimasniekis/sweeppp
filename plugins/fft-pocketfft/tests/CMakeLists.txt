# The PocketFFT plugin's own suite.
#
# Beside the plugin rather than in the top-level tests/ directory: what a
# plugin promises is the plugin's business, and a test that lives next to the
# code it covers is the one that gets updated when that code moves.
#
# It goes through the real loader rather than compiling the backend in, because
# there is an ABI here: the transform reaches the pipeline through a vtable,
# and "the facet registers as `pocketfft`" is a promise about the built module
# rather than about the source.
add_executable(sweeppp-plugin-fft-pocketfft-tests
    main.cpp
    test_pocketfft_plugin.cpp)

target_link_libraries(sweeppp-plugin-fft-pocketfft-tests PRIVATE
    sweeppp::sweeppp
    doctest::doctest
    sweeppp_warnings)

target_compile_definitions(sweeppp-plugin-fft-pocketfft-tests PRIVATE
    DOCTEST_CONFIG_DOUBLE_STRINGIFY
    SWEEPPP_PLUGIN_BINARY_DIR="${SWEEPPP_OUTPUT_DIR}/plugins")

# The module has to exist before the suite runs, and nothing links the two.
add_dependencies(sweeppp-plugin-fft-pocketfft-tests sweeppp_plugin_fft_pocketfft)

# The benchmark case compares against whichever other backends are built. It
# discovers them through the same directory scan, so these are ordering
# dependencies and not link ones.
if(TARGET sweeppp_plugin_fft_fftw)
    add_dependencies(sweeppp-plugin-fft-pocketfft-tests sweeppp_plugin_fft_fftw)
endif()
if(TARGET sweeppp_plugin_fft_accelerate)
    add_dependencies(sweeppp-plugin-fft-pocketfft-tests sweeppp_plugin_fft_accelerate)
endif()

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

doctest_discover_tests(sweeppp-plugin-fft-pocketfft-tests
    PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
