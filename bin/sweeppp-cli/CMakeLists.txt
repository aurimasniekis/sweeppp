add_executable(sweeppp-cli
    src/main.cpp
    src/Options.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/WebAssets.cpp")

target_include_directories(sweeppp-cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# sweeps::cli-core carries the session commands. `sweeppp-cli info` and
# `sweeps info` are then the same code producing the same text, rather than two
# implementations that happen to agree until one is edited.
target_link_libraries(sweeppp-cli PRIVATE
    sweeppp::sweeppp
    sweeps::cli-core
    sweeppp_warnings)

# The browser UI `serve --web` hands out: built by pnpm from web/, then
# compiled in, so the binary is all a server needs. Without it, the table is
# empty and `serve --web` says the UI was not built.
set(_sweeppp_web_table "${CMAKE_CURRENT_BINARY_DIR}/WebAssets.cpp")
# -P last: CMake reads anything after it as the script's own arguments.
set(_sweeppp_embed_script -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake")
set(_sweeppp_embed
    "${CMAKE_COMMAND}" -DOUTPUT=${_sweeppp_web_table}
    -DFUNCTION=sweeppp::cli::webAssets -DHEADER=WebAssets.hpp)

if(SWEEPPP_WITH_WEB_UI)
    find_program(SWEEPPP_PNPM pnpm)
    if(NOT SWEEPPP_PNPM)
        message(FATAL_ERROR
            "The browser UI is built with pnpm, which was not found.\n"
            "  Node.js 20 or newer:  corepack enable pnpm\n"
            "  or:                   brew install pnpm\n"
            "Or build without it: -DSWEEPPP_WITH_WEB_UI=OFF")
    endif()

    set(_web "${PROJECT_SOURCE_DIR}/web")
    set(_web_out "${CMAKE_BINARY_DIR}/web-dist")
    file(GLOB_RECURSE _web_sources CONFIGURE_DEPENDS "${_web}/src/*")
    list(APPEND _web_sources "${_web}/index.html" "${_web}/vite.config.ts"
        "${_web}/tsconfig.json")

    # Dependencies come from the lockfile only, and again only when it moves.
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/web-install.stamp"
        COMMAND "${SWEEPPP_PNPM}" install --frozen-lockfile
        COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/web-install.stamp"
        DEPENDS "${_web}/package.json" "${_web}/pnpm-lock.yaml"
        WORKING_DIRECTORY "${_web}"
        COMMENT "Installing the browser UI's dependencies"
        VERBATIM)
    add_custom_command(
        OUTPUT "${_web_out}/index.html"
        COMMAND "${CMAKE_COMMAND}" -E env "SWEEPPP_WEB_OUT=${_web_out}" "${SWEEPPP_PNPM}" build
        DEPENDS "${CMAKE_BINARY_DIR}/web-install.stamp" ${_web_sources}
        WORKING_DIRECTORY "${_web}"
        COMMENT "Building the browser UI"
        VERBATIM)
    add_custom_command(
        OUTPUT ${_sweeppp_web_table}
        COMMAND ${_sweeppp_embed} -DDIR=${_web_out} ${_sweeppp_embed_script}
        DEPENDS "${_web_out}/index.html" "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
        COMMENT "Embedding the browser UI"
        VERBATIM)
    target_compile_definitions(sweeppp-cli PRIVATE SWEEPPP_HAVE_WEB_UI=1)
else()
    add_custom_command(
        OUTPUT ${_sweeppp_web_table}
        COMMAND ${_sweeppp_embed} ${_sweeppp_embed_script}
        DEPENDS "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
        VERBATIM)
endif()
