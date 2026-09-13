# The Accelerate plugin's own suite.
#
# Beside the plugin rather than in the top-level tests/ directory: what a
# plugin promises is the plugin's business, and a test that lives next to the
# code it covers is the one that gets updated when that code moves.
#
# It goes through the real loader rather than compiling the backend in, because
# there is an ABI here: the transform reaches the pipeline through a vtable,
# and "the facet registers as `accelerate`" is a promise about the built module
# rather than about the source.
add_executable(sweeppp-plugin-fft-accelerate-tests
    main.cpp
    test_accelerate_plugin.cpp)

target_link_libraries(sweeppp-plugin-fft-accelerate-tests PRIVATE
    sweeppp::sweeppp
    doctest::doctest
    sweeppp_warnings)

target_compile_definitions(sweeppp-plugin-fft-accelerate-tests PRIVATE
    DOCTEST_CONFIG_DOUBLE_STRINGIFY
    SWEEPPP_PLUGIN_BINARY_DIR="${SWEEPPP_OUTPUT_DIR}/plugins")

# The module has to exist before the suite runs, and nothing links the two.
add_dependencies(sweeppp-plugin-fft-accelerate-tests sweeppp_plugin_fft_accelerate)

# The benchmark case compares against FFTW when it is present. It discovers the
# module through the same directory scan as everything else, so this is an
# ordering dependency and not a link one -- and it is conditional, because the
# case reports what it can on a build with no FFTW.
if(TARGET sweeppp_plugin_fft_fftw)
    add_dependencies(sweeppp-plugin-fft-accelerate-tests sweeppp_plugin_fft_fftw)
endif()

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

doctest_discover_tests(sweeppp-plugin-fft-accelerate-tests
    PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
