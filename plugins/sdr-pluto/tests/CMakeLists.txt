# The PlutoSDR plugin's own suite.
#
# It goes through the real loader rather than compiling the driver in, because
# there is an ABI here: "the facet registers as `pluto`" is a promise about the
# built module rather than about the source.
add_executable(sweeppp-plugin-sdr-pluto-tests
    main.cpp
    test_pluto_parse.cpp
    test_pluto_plugin.cpp)

target_link_libraries(sweeppp-plugin-sdr-pluto-tests PRIVATE
    sweeppp::sweeppp
    doctest::doctest
    sweeppp_warnings)

target_compile_definitions(sweeppp-plugin-sdr-pluto-tests PRIVATE
    DOCTEST_CONFIG_DOUBLE_STRINGIFY
    SWEEPPP_PLUGIN_BINARY_DIR="${SWEEPPP_OUTPUT_DIR}/plugins")

# PlutoParse.hpp is header-only and needs no libiio.
target_include_directories(sweeppp-plugin-sdr-pluto-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# The module has to exist before the suite runs, and nothing links the two.
add_dependencies(sweeppp-plugin-sdr-pluto-tests sweeppp_plugin_sdr_pluto)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

doctest_discover_tests(sweeppp-plugin-sdr-pluto-tests
    PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
