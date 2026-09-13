# Warning flags applied only to our own targets, never to vendored sources.
# Use via: target_link_libraries(<tgt> PRIVATE sweeppp_warnings)

add_library(sweeppp_warnings INTERFACE)

if(MSVC)
    # /utf-8: the sources are UTF-8 and a few UI strings carry arrows and
    # triangles as \u escapes. Without it cl reads and writes them in the
    # host's code page, warns C4566, and the GUI shows '?' where the glyph
    # should be.
    #
    # C4324 is "structure was padded due to alignment specifier", reported
    # for every alignas(64) on the lock-free queues and counters -- the
    # padding is the point.
    target_compile_options(sweeppp_warnings INTERFACE /W4 /permissive- /utf-8 /wd4324)
    # getenv is the portable spelling and the CRT's _dupenv_s is not; the
    # deprecation is Microsoft's opinion, not a defect.
    target_compile_definitions(sweeppp_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(SWEEPPP_WERROR)
        target_compile_options(sweeppp_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(sweeppp_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough)

    # Clang 19 added -Wmissing-designated-field-initializers to -Wextra. It
    # fires on every designated initialiser that leaves a field to its default
    # member initialiser, which is the deliberate idiom here and in ~370 places
    # -- naming each remaining field would say less, not more. The warning it
    # replaces for non-designated aggregates, -Wmissing-field-initializers,
    # stays on for Clang.
    #
    # GCC has no such split: its -Wmissing-field-initializers fires on the
    # designated form too, at every one of those sites, so there it goes off
    # altogether. Clang still checks the non-designated aggregates on macOS
    # and in the sanitiser jobs.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(sweeppp_warnings INTERFACE
            -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
            -Wno-missing-field-initializers)
    endif()

    #
    # Probed in its positive form: Clang accepts an unknown -Wno-* silently and
    # only complains once some other warning fires in the same file, so testing
    # -Wno-... directly would succeed everywhere and tell us nothing.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wmissing-designated-field-initializers
        SWEEPPP_HAVE_WMISSING_DESIGNATED)
    if(SWEEPPP_HAVE_WMISSING_DESIGNATED)
        target_compile_options(sweeppp_warnings INTERFACE
            -Wno-missing-designated-field-initializers)
    endif()

    # doctest::Approx takes a double and this codebase measures in float, so
    # every CHECK(someFloat == Approx(x)) promotes -- 58 of them, and the
    # promotion is the comparison working as intended. -Wdouble-promotion earns
    # its place in the DSP code, where an unintended double is a real cost, and
    # that code is clean, so the warning is turned off for test targets rather
    # than dropped.
    #
    # Keyed on linking doctest, because that is exactly the set of test targets
    # and needs no list for anyone to forget to update. It has to come after
    # -Wdouble-promotion above to win, which is why it lives here and not on
    # doctest's own interface -- every suite links doctest first and
    # sweeppp_warnings last, so a flag set over there is overridden again.
    #
    # Clang 19 reports these and Apple clang does not, which is why the suites
    # build clean on macOS and broke the Linux sanitiser jobs.
    target_compile_options(sweeppp_warnings INTERFACE
        "$<$<IN_LIST:doctest::doctest,$<TARGET_PROPERTY:LINK_LIBRARIES>>:-Wno-double-promotion>")

    if(SWEEPPP_WERROR)
        target_compile_options(sweeppp_warnings INTERFACE -Werror)
    endif()
endif()
