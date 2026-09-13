# The conformance suite.
#
# doctest is fetched here rather than assumed, so this directory configures on
# its own with no Sweep++ present. When Sweep++ has already made the target
# available, that one is reused.

if(NOT TARGET doctest::doctest)
    include(FetchContent)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz)
    FetchContent_MakeAvailable(doctest)

    # doctest forward-declares std::basic_ostream instead of including
    # <ostream>, and turns that off by itself only for libc++. MSVC's
    # <string_view> defines operator<<(ostream&, string_view) inline and needs
    # the complete type, so stringifying a string_view in any CHECK fails
    # inside MSVC's own headers. Set on the target rather than per test, so a
    # suite added later inherits it. Sweep++ does the same to the target it
    # provides, which is the one reused above.
    target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)
endif()

set(SWEEPSFILE_TEST_SOURCES
    main.cpp
    test_capi.cpp
    test_golden.cpp
    test_mappedfile.cpp
    test_metadata.cpp
    test_session.cpp
    test_text.cpp
    test_version.cpp
)

add_executable(sweepsfile-tests ${SWEEPSFILE_TEST_SOURCES})

set_target_properties(sweepsfile-tests PROPERTIES
    CXX_STANDARD ${SWEEPSFILE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

target_link_libraries(sweepsfile-tests PRIVATE sweeps::sweepsfile doctest::doctest)

if(NOT MSVC)
    target_compile_options(sweepsfile-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# doctest stringifies operands through an unqualified toString(), which ADL
# resolves to sweeps::toString(ErrorCode). That returns std::string_view rather
# than doctest::String, so the single-stringify form fails to compile on any
# CHECK comparing our enums. The double form wraps the result in doctest's own
# stringifier -- and a failing CHECK then prints "Corrupt" rather than an
# opaque integer.
target_compile_definitions(sweepsfile-tests PRIVATE DOCTEST_CONFIG_DOUBLE_STRINGIFY)

target_compile_definitions(sweepsfile-tests PRIVATE
    SWEEPSFILE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")

doctest_discover_tests(sweepsfile-tests
    PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# ---------------------------------------------------------------------------
# The C ABI, compiled as C.
#
# A separate executable, because the C++ suite cannot host it: compiling this
# translation unit with a C compiler is the entire test. `sweeps/sweeps.h`
# quietly acquiring a `namespace`, a default argument or a `nullptr` would be
# invisible to every doctest case and fatal to every binding.
#
# Registered with a plain add_test(): doctest_discover_tests() runs the binary
# with --list-test-cases, which this one does not understand.
# ---------------------------------------------------------------------------
enable_language(C)

add_executable(sweepsfile-capi-tests test_capi.c)

set_target_properties(sweepsfile-capi-tests PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    # The library is C++ however it is built, so the executable needs the C++
    # driver to pull in the runtime it depends on. CMake works this out on its
    # own for an in-tree target; stating it keeps a C-only consumer's link from
    # depending on that inference. See tests/consumer-c for the installed case,
    # where the inference is not available at all.
    LINKER_LANGUAGE CXX)

target_link_libraries(sweepsfile-capi-tests PRIVATE sweeps::sweepsfile)

if(NOT MSVC)
    target_compile_options(sweepsfile-capi-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(sweepsfile-capi-tests PRIVATE
    SWEEPSFILE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_test(NAME capi COMMAND sweepsfile-capi-tests)
set_tests_properties(capi PROPERTIES ENVIRONMENT
    "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# ---------------------------------------------------------------------------
# The Python binding.
#
# Registered only for a shared build, because ctypes has nothing to load out of
# a static archive -- against one this would be a guaranteed failure rather than
# a check. `make python` is the shared build that runs it.
#
# The interpreter is optional. A machine without Python 3 can still build and
# test the library; it just cannot check the binding, and says so rather than
# failing to configure.
# ---------------------------------------------------------------------------
if(BUILD_SHARED_LIBS)
    find_package(Python3 COMPONENTS Interpreter QUIET)

    if(Python3_Interpreter_FOUND)
        set(SWEEPSFILE_PYTHON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../python")

        add_test(NAME python
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s tests -t .
            WORKING_DIRECTORY "${SWEEPSFILE_PYTHON_DIR}")

        # The library under test is this build's, named explicitly: the search
        # in sweepsfile._ffi would otherwise be free to find an installed one,
        # and a test that checks a different binary than the one just compiled
        # is worse than no test.
        set_tests_properties(python PROPERTIES ENVIRONMENT
            "SWEEPSFILE_LIBRARY=$<TARGET_FILE:sweepsfile>;SWEEPSFILE_TEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data")
    else()
        message(STATUS "libsweepsfile: no Python 3 interpreter, skipping the ctypes binding test")
    endif()
endif()

# ---------------------------------------------------------------------------
# The install round-trips.
#
# "It installs" and "a consumer can link it" are claims that are only true if
# something checks them, and neither is visible from inside the build tree:
# every test above compiles against the source directory, so an install rule
# that dropped a header would leave all of them passing.
#
# Registered as ctest cases rather than driven from a Makefile above this
# directory, so the library carries its own proof. Standalone builds only --
# under add_subdirectory() `cmake --install` would install the *parent* project,
# which is emphatically not what this is asking about.
# ---------------------------------------------------------------------------
if(SWEEPSFILE_STANDALONE AND SWEEPSFILE_INSTALL)
    set(SWEEPSFILE_TEST_PREFIX "${CMAKE_BINARY_DIR}/test-prefix")

    add_test(NAME install
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
                --prefix "${SWEEPSFILE_TEST_PREFIX}" --config $<CONFIG>)
    set_tests_properties(install PROPERTIES FIXTURES_SETUP installed)

    # A fixture rather than test ordering by hope: both consumers need the
    # install to have happened, and ctest is free to run tests in parallel.
    function(sweepsfile_add_consumer name directory executable)
        add_test(NAME ${name}
            COMMAND "${CMAKE_COMMAND}"
                    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/${directory}"
                    "-DBINARY_DIR=${CMAKE_BINARY_DIR}/${directory}"
                    "-DPREFIX=${SWEEPSFILE_TEST_PREFIX}"
                    "-DEXECUTABLE=${executable}"
                    "-DGENERATOR=${CMAKE_GENERATOR}"
                    "-DBUILD_TYPE=$<CONFIG>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/RunConsumer.cmake")
        set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED installed)
    endfunction()

    # Two of them, because they prove different things. The C++ one checks the
    # export carries its own language standard. The C one checks that
    # `sweeps/sweeps.h` is actually installed, and that a project with no C++
    # compiler enabled can still link a library made entirely of C++ -- the two
    # ways the C ABI can be published and be useless.
    sweepsfile_add_consumer(consumer-cxx consumer sweeps-consumer)
    sweepsfile_add_consumer(consumer-c consumer-c sweeps-consumer-c)
endif()
