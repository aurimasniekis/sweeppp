# The detections plugin's own suite.
#
# Beside the plugin rather than in the top-level tests/ directory, for the same
# reason the band plan's and the channel list's are: what a plugin promises is
# the plugin's business, and a test that lives next to the code it covers is
# the one that gets updated when that code moves.
#
# It compiles the model directly instead of loading the built module. There is
# no ABI in `Detector` -- it takes a span of floats and a timestamp -- so going
# through `dlopen` would test the host, which tests/test_plugin.cpp already
# does with purpose-built fixtures.

add_executable(sweeppp-plugin-detections-tests
    main.cpp
    test_detections.cpp
    ../src/Detector.cpp)

target_include_directories(sweeppp-plugin-detections-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

target_link_libraries(sweeppp-plugin-detections-tests PRIVATE
    sweeppp::sweeppp
    doctest::doctest
    sweeppp_warnings)

target_compile_definitions(sweeppp-plugin-detections-tests PRIVATE
    DOCTEST_CONFIG_DOUBLE_STRINGIFY)

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

doctest_discover_tests(sweeppp-plugin-detections-tests
    PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
