# UI, but not for drawing the spans: the host still does that from what the
# contributor facet hands it. What this compiles ImGui for is the tree that
# decides which services are handed over at all.
sweeppp_add_plugin(sweeppp_plugin_bandplan
    NAME bandplan
    UI
    SOURCES
        src/BandPlan.cpp
        src/BandPlanPlugin.cpp
    # For values only -- Color, toml_util, Result, PluginSettings. The rule the
    # plugin must not break is at the top of <sweeppp/plugin/Plugin.hpp>: link
    # libsweeppp for values, never touch a singleton.
    LINK sweeppp::sweeppp)

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
