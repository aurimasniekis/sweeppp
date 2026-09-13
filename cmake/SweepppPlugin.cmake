# Adding a plugin.
#
# Every plugin needs the same six things right -- MODULE, no `lib` prefix, the
# dist/plugins output directory, hidden visibility, the ImGui *headers with the
# host's compile definitions but not the host's ImGui objects*, and
# SWEEPPP_PLUGIN_HAS_UI. Copying that block per plugin is how one of them ends
# up built without `sweeppp_imconfig.h`, which corrupts every draw list it
# touches rather than failing.
#
#   sweeppp_add_plugin(sweeppp_plugin_bandplan
#       NAME bandplan                   # -> sweeppp-plugin-bandplan.so
#       UI
#       SOURCES src/BandPlanPlugin.cpp src/BandPlan.cpp
#       LINK    sweeppp::sweeppp)
#
# NAME   the plugin's short name. The file gets the `sweeppp-plugin-` prefix
#        the host looks for in shared library directories -- see
#        SWEEPPP_PLUGIN_FILE_PREFIX in PluginAbi.h. Defaulted rather than
#        optional, because a plugin that is only ever installed into a
#        directory of ours works either way and one that is packaged does not.
# OUTPUT_NAME  the exact filename, prefix and all, for a plugin that has a
#        reason to be called something else.
# UI     the plugin draws, so it gets the ImGui/ImPlot headers and the host's
#        IMGUI_USER_CONFIG. Silently ignored when the GUI is not being built,
#        so a headless configuration still produces the plugin -- its UI facet
#        is then listed as unsupported rather than missing.
# LINK   extra libraries. `sweeppp::sweeppp` is allowed and is what gives a
#        plugin Color, toml_util and Result; see the rule at the top of
#        <sweeppp/plugin/Plugin.hpp> about values versus singletons.
function(sweeppp_add_plugin target)
    cmake_parse_arguments(PLUGIN "UI" "NAME;OUTPUT_NAME" "SOURCES;LINK;INCLUDE" ${ARGN})

    if(NOT PLUGIN_SOURCES)
        message(FATAL_ERROR "sweeppp_add_plugin(${target}) needs SOURCES")
    endif()

    if(NOT PLUGIN_OUTPUT_NAME)
        if(NOT PLUGIN_NAME)
            set(PLUGIN_NAME "${target}")
        endif()
        set(PLUGIN_OUTPUT_NAME "sweeppp-plugin-${PLUGIN_NAME}")
    endif()

    add_library(${target} MODULE ${PLUGIN_SOURCES})

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${PLUGIN_OUTPUT_NAME}"
        PREFIX ""
        # Hidden by default, so the only exported symbols are the two the ABI
        # names. On ELF an exported ImGui symbol from a plugin can interpose on
        # the host's copy depending on link flags; two deliberate copies
        # sharing one context is the supported arrangement, one copy sometimes
        # silently replacing the other is not.
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY "${SWEEPPP_OUTPUT_DIR}/plugins")

    target_link_libraries(${target} PRIVATE sweeppp_warnings ${PLUGIN_LINK})

    if(PLUGIN_INCLUDE)
        target_include_directories(${target} PRIVATE ${PLUGIN_INCLUDE})
    endif()

    # The plugin ABI header, so a plugin that links nothing of ours still
    # compiles.
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/lib/libsweeppp/include")

    if(PLUGIN_UI AND SWEEPPP_BUILD_GUI)
        # Headers and compile definitions, never the objects: the plugin
        # compiles its own ImGui or links one of its own, and adopts the host's
        # context and allocators at activation. The definitions are the
        # load-bearing half -- IMGUI_USER_CONFIG="sweeppp_imconfig.h" is what
        # makes ImDrawIdx 32 bits on both sides.
        target_include_directories(${target} SYSTEM PRIVATE
            $<TARGET_PROPERTY:sweeppp_imgui,INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_definitions(${target} PRIVATE
            $<TARGET_PROPERTY:sweeppp_imgui,INTERFACE_COMPILE_DEFINITIONS>)
        target_compile_definitions(${target} PRIVATE SWEEPPP_PLUGIN_HAS_UI=1)

        # The plugin's own copy of ImGui and ImPlot.
        #
        # Not the host's static library: linking that would put a second set of
        # ImGui globals in the process with no way to tell which copy a call
        # reached. A private copy with hidden visibility, sharing the host's
        # context and allocators, is ImGui's own documented arrangement for a
        # dynamically loaded module.
        target_sources(${target} PRIVATE
            "${imgui_SOURCE_DIR}/imgui.cpp"
            "${imgui_SOURCE_DIR}/imgui_draw.cpp"
            "${imgui_SOURCE_DIR}/imgui_tables.cpp"
            "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
            "${implot_SOURCE_DIR}/implot.cpp"
            "${implot_SOURCE_DIR}/implot_items.cpp")

        # Vendored sources, so our warning set does not drown the build.
        if(NOT MSVC)
            set_source_files_properties(
                "${imgui_SOURCE_DIR}/imgui.cpp"
                "${imgui_SOURCE_DIR}/imgui_draw.cpp"
                "${imgui_SOURCE_DIR}/imgui_tables.cpp"
                "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
                "${implot_SOURCE_DIR}/implot.cpp"
                "${implot_SOURCE_DIR}/implot_items.cpp"
                TARGET_DIRECTORY ${target}
                PROPERTIES COMPILE_OPTIONS "-w")
        endif()
    endif()
endfunction()
