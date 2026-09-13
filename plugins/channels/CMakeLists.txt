# UI, unlike the band plan: this plugin draws a tree of its own and a window of
# its own, which is the case the UI facet exists for. Its contributor facet
# still ships no drawing code -- the host paints the spans.
sweeppp_add_plugin(sweeppp_plugin_channels
    NAME channels
    UI
    SOURCES
        src/ChannelSet.cpp
        src/ChannelsPlugin.cpp
    # For values only -- Color, toml_util, Result, PluginSettings. The rule the
    # plugin must not break is at the top of <sweeppp/plugin/Plugin.hpp>: link
    # libsweeppp for values, never touch a singleton.
    LINK sweeppp::sweeppp)

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
