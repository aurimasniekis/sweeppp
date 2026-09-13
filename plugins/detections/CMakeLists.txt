# The first frame processor in the tree, and the case that facet exists for: it
# reads every published spectrum on a thread the host owns, and answers
# questions about what was in them long after the frames themselves are gone.
#
# UI as well, because the list is the feature. A plugin built with UI compiles
# its own ImGui and ImPlot, which is what lets each row carry a sparkline
# without the host lending it anything.
sweeppp_add_plugin(sweeppp_plugin_detections
    NAME detections
    UI
    SOURCES
        src/Detector.cpp
        src/DetectionsPlugin.cpp
    # For values only -- toml_util, Result, PluginSettings, Clock, Telemetry.
    # The rule the plugin must not break is at the top of
    # <sweeppp/plugin/Plugin.hpp>: link libsweeppp for values, never touch a
    # singleton.
    LINK sweeppp::sweeppp)

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
