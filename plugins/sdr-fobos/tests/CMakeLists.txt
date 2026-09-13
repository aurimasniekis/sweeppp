# The Fobos SDR plugin's own suite.
#
# Beside the plugin rather than in the top-level tests/ directory: what a
# plugin promises is the plugin's business, and a test that lives next to the
# code it covers is the one that gets updated when that code moves.
#
# It goes through the real loader rather than compiling the driver in, because
# there is an ABI here: the driver reaches the registry through a vtable, and
# "the facet registers as `fobos`" is a promise about the built module rather
# than about the source.
add_executable(sweeppp-plugin-sdr-fobos-tests
    main.cpp
    test_fobos_plugin.cpp
    test_fobos_serials.cpp)

target_link_libraries(sweeppp-plugin-sdr-fobos-tests PRIVATE
    sweeppp::sweeppp
    doctest::doctest
    sweeppp_warnings)

target_compile_definitions(sweeppp-plugin-sdr-fobos-tests PRIVATE
    DOCTEST_CONFIG_DOUBLE_STRINGIFY
    SWEEPPP_PLUGIN_BINARY_DIR="${SWEEPPP_OUTPUT_DIR}/plugins")

# FobosSerials.hpp is header-only and needs neither library.
target_include_directories(sweeppp-plugin-sdr-fobos-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# The module has to exist before the suite runs, and nothing links the two.
add_dependencies(sweeppp-plugin-sdr-fobos-tests sweeppp_plugin_sdr_fobos)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

doctest_discover_tests(sweeppp-plugin-sdr-fobos-tests
    PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
