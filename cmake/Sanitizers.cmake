# Sanitizers are applied globally (compile *and* link) because a sanitized
# binary must have every translation unit instrumented, vendored code included.

if(NOT SWEEPPP_SANITIZER STREQUAL "")
    if(MSVC)
        message(FATAL_ERROR "SWEEPPP_SANITIZER is not supported with MSVC")
    endif()

    set(_san_flags "")
    if(SWEEPPP_SANITIZER MATCHES "address")
        list(APPEND _san_flags address)
    endif()
    if(SWEEPPP_SANITIZER MATCHES "thread")
        list(APPEND _san_flags thread)
    endif()
    if(SWEEPPP_SANITIZER MATCHES "undefined")
        list(APPEND _san_flags undefined)
    endif()

    if(_san_flags STREQUAL "")
        message(FATAL_ERROR "Unrecognised SWEEPPP_SANITIZER='${SWEEPPP_SANITIZER}'")
    endif()

    if("address" IN_LIST _san_flags AND "thread" IN_LIST _san_flags)
        message(FATAL_ERROR "ASan and TSan cannot be combined")
    endif()

    list(JOIN _san_flags "," _san_joined)
    add_compile_options(-fsanitize=${_san_joined} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${_san_joined})

    # Vendored code is excluded with specific, documented reasons -- see the
    # ignore list. A sanitized build is only useful if its output is signal,
    # and hundreds of identical reports from inside FFTW are not.
    set(_san_ignorelist "${CMAKE_CURRENT_LIST_DIR}/sanitizer-ignorelist.txt")
    if(EXISTS "${_san_ignorelist}")
        add_compile_options("-fsanitize-ignorelist=${_san_ignorelist}")
    endif()

    message(STATUS "Sanitizers enabled: ${_san_joined}")
endif()
