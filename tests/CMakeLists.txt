set(SWEEPPP_TEST_SOURCES
    main.cpp
    ReferenceFft.cpp
    test_antenna.cpp
    test_core.cpp
    test_correction.cpp
    test_fft.cpp
    test_format.cpp
    test_golden.cpp
    test_marker.cpp
    test_panels.cpp
    test_pipeline.cpp
    test_plugin.cpp
    test_session.cpp
    test_profile.cpp
    test_sweep.cpp
    test_theme.cpp
    test_toast.cpp
    test_tracestore.cpp
    test_toml.cpp
    test_update.cpp
    test_window.cpp

    # From the GUI, and compiled here *without* SWEEPPP_WITH_UPDATE_CHECK, so
    # the suite needs neither libcurl nor a network. What is worth testing is
    # the version comparison, and that is the half that has no transport in it.
    "${CMAKE_SOURCE_DIR}/bin/sweeppp/src/UpdateCheck.cpp"
)

# This suite covers the host. Each plugin's own lives beside it, the way
# libsweepsfile's does, and goes through the loader it exercises -- so nothing
# here depends on which plugins a build produced. `ReferenceFft.cpp` is the
# transform that independence needs.

add_executable(sweeppp-tests ${SWEEPPP_TEST_SOURCES})

target_include_directories(sweeppp-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/bin/sweeppp/src"
    "${CMAKE_SOURCE_DIR}/lib/libsweeppp/src")

target_link_libraries(sweeppp-tests PRIVATE
    sweeppp::sweeppp
    doctest::doctest
    sweeppp_warnings)

# doctest stringifies operands by calling an unqualified toString(), which ADL
# resolves to our own sweeppp::toString(WindowType) etc. Those return
# std::string_view, not doctest::String, so the single-stringify form fails to
# compile on any CHECK comparing our enums. The double form wraps the result in
# doctest's own stringifier -- and as a bonus, a failing CHECK then prints
# "hann" rather than an opaque integer.
target_compile_definitions(sweeppp-tests PRIVATE DOCTEST_CONFIG_DOUBLE_STRINGIFY)

# The frozen reference session lives with libsweepsfile's conformance suite.
# Sweep++'s tests read the same file, so the recorder and the library's own
# synchronous writer are checked against one reference rather than two that
# could drift apart.
target_compile_definitions(sweeppp-tests PRIVATE
    SWEEPSFILE_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/lib/libsweepsfile/tests/data")

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

# A sanitiser finding must fail the test that provoked it.
#
# Both default to reporting and carrying on, which means a TSan build can print
# a data race and still exit 0 -- so the suite goes green while the thing the
# preset exists to catch scrolls past in the log. Exactly that hid a race
# between the sweep thread and the session writer.
doctest_discover_tests(sweeppp-tests
    PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
