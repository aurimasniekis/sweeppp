sweeppp_add_plugin(sweeppp_plugin_sdr_fobos
    NAME sdr-fobos
    SOURCES
        src/FobosAgile.cpp
        src/FobosDevice.cpp
        src/FobosPlugin.cpp
        src/FobosStandard.cpp
    # No UI: this driver draws nothing. The SDR panel is generated from the
    # device's own parameters(), which is what lets a driver ship without a
    # line of interface code.
    #
    # libsweeppp is for VALUES only -- SdrParameter, SampleFormat, Result. The
    # rule the plugin must not break is at the top of
    # <sweeppp/plugin/Plugin.hpp>: link libsweeppp for values, never touch a
    # singleton. SdrDeviceManager::instance() here would be a private registry
    # the host never sees.
    #
    # Both Fobos libraries are compiled into this module rather than loaded
    # beside it; see the block in cmake/Dependencies.cmake that builds them.
    LINK sweeppp_vendor_fobos sweeppp::sweeppp)

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
