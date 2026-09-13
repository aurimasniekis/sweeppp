# The band plan plugin's own suite.
#
# Beside the plugin rather than in the top-level tests/ directory, the same way
# libsweepsfile keeps its conformance suite: what a plugin promises is the
# plugin's business, and a test that lives next to the code it covers is the
# one that gets updated when that code moves.
#
# It compiles the loader directly instead of loading the built module. There is
# no ABI in `BandPlan` -- it is a value type over toml_util -- so going through
# `dlopen` would test the host, which tests/test_plugin.cpp already does with
# purpose-built fixtures.

add_executable(sweeppp-plugin-bandplan-tests
    main.cpp
    test_bandplan.cpp
    ../src/BandPlan.cpp)

target_include_directories(sweeppp-plugin-bandplan-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

target_link_libraries(sweeppp-plugin-bandplan-tests PRIVATE
    sweeppp::sweeppp
    doctest::doctest
    sweeppp_warnings)

target_compile_definitions(sweeppp-plugin-bandplan-tests PRIVATE
    DOCTEST_CONFIG_DOUBLE_STRINGIFY)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

doctest_discover_tests(sweeppp-plugin-bandplan-tests
    PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
