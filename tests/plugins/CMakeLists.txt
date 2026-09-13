# Fixture plugins.
#
# Built whenever the tests are, whether or not SWEEPPP_BUILD_PLUGINS is on:
# these are test inputs rather than shipped plugins, and a `dlopen` of
# instrumented code is exactly what the sanitiser presets exist to watch. They
# land outside dist/plugins so the real application never loads them.
set(SWEEPPP_TEST_PLUGIN_DIR "${CMAKE_BINARY_DIR}/test-plugins")

# A plugin that works, and one that duplicates its id.
#
# `dupe` is not a copy of `good`: the id is the same and the file is different,
# which is the situation the "first wins, the other is listed as shadowed" rule
# is about.
function(sweeppp_add_fixture_plugin name)
    cmake_parse_arguments(FIXTURE "" "" "SOURCES;DEFINES" ${ARGN})

    add_library(${name} MODULE ${FIXTURE_SOURCES})
    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY "${SWEEPPP_TEST_PLUGIN_DIR}")

    target_include_directories(${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/lib/libsweeppp/include")

    if(FIXTURE_DEFINES)
        target_compile_definitions(${name} PRIVATE ${FIXTURE_DEFINES})
    endif()

    add_dependencies(sweeppp-tests ${name})
endfunction()

sweeppp_add_fixture_plugin(fixture_good
    SOURCES Fixture.cpp
    DEFINES FIXTURE_ID="test.sweeppp.good")

sweeppp_add_fixture_plugin(fixture_dupe
    SOURCES Fixture.cpp
    DEFINES FIXTURE_ID="test.sweeppp.good" FIXTURE_MIN_HOST="0.0.2")

# A second contributor, over the same frequencies and with a different type.
#
# Ordering is a question about several contributors, and one plugin cannot
# answer it: with only `good` installed, "who titles the marker" has one
# possible answer whatever the operator's order says. Contributor-only so the
# rest of the suite -- which counts drivers, backends and facets -- sees
# exactly the world it saw before.
sweeppp_add_fixture_plugin(fixture_contributor
    SOURCES Fixture.cpp
    DEFINES FIXTURE_ID="test.sweeppp.contributor" FIXTURE_CONTRIBUTOR_ONLY=1
            FIXTURE_CONTRIBUTION=SWEEPPP_CONTRIBUTION_CHANNEL)

# ABI too new, and ABI too old. Both must be reported as a version mismatch
# naming both numbers rather than as a plugin that merely refused.
sweeppp_add_fixture_plugin(fixture_newabi
    SOURCES Fixture.cpp
    DEFINES FIXTURE_ID="test.sweeppp.newabi" FIXTURE_ABI=99)

sweeppp_add_fixture_plugin(fixture_oldabi
    SOURCES Fixture.cpp
    DEFINES FIXTURE_ID="test.sweeppp.oldabi" FIXTURE_ABI=0)

# A plugin from the future: the ABI matches, the application does not.
sweeppp_add_fixture_plugin(fixture_newhost
    SOURCES Fixture.cpp
    DEFINES FIXTURE_ID="test.sweeppp.newhost" FIXTURE_MIN_HOST="99.0.0")

sweeppp_add_fixture_plugin(fixture_needsdep
    SOURCES Fixture.cpp
    DEFINES FIXTURE_ID="test.sweeppp.needsdep" FIXTURE_DEPENDENCY="test.sweeppp.absent")

sweeppp_add_fixture_plugin(fixture_nosym
    SOURCES NoEntryPoint.cpp)

target_compile_definitions(sweeppp-tests PRIVATE
    SWEEPPP_TEST_PLUGIN_DIR="${SWEEPPP_TEST_PLUGIN_DIR}")
