sweeppp_add_plugin(sweeppp_plugin_sdr_pluto
    NAME sdr-pluto
    SOURCES
        src/PlutoDevice.cpp
        src/PlutoPlugin.cpp
    # No UI: this driver draws nothing. The SDR panel is generated from the
    # device's own parameters(), which is what lets a driver ship without a
    # line of interface code.
    #
    # libsweeppp is for VALUES only -- SdrParameter, SampleFormat, Result. The
    # rule the plugin must not break is at the top of
    # <sweeppp/plugin/Plugin.hpp>: link libsweeppp for values, never touch a
    # singleton.
    #
    # sweeppp_iio is the system libiio, or on macOS a copy compiled into this
    # module; see the block in cmake/Dependencies.cmake that decides.
    LINK sweeppp_iio sweeppp::sweeppp)

if(SWEEPPP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
