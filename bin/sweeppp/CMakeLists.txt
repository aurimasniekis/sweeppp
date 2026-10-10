set(SWEEPPP_GUI_SOURCES
    src/main.cpp
    src/AppWindow.cpp
    src/FileDialog.cpp
    src/Icons.cpp
    src/AppSettings.cpp
    src/AppState.cpp
    src/ContributionOverlay.cpp
    src/FftBenchmarkRunner.cpp
    src/MainWindow.cpp
    src/MainWindowPlots.cpp
    src/MainWindowPanels.cpp
    src/MainWindowViews.cpp
    src/MainWindowWhatsNew.cpp
    src/Snapshot.cpp
    src/UpdateCheck.cpp
    src/render/HistoryView.cpp
    src/render/ImageTexture.cpp
    src/render/WaterfallRenderer.cpp
)

add_executable(sweeppp-gui ${SWEEPPP_GUI_SOURCES})
set_target_properties(sweeppp-gui PROPERTIES OUTPUT_NAME sweeppp)

target_include_directories(sweeppp-gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The executable's icon: the channel's mark, branding/<SWEEPPP_APP_ID>.ico.
# Windows is the only platform that carries one inside the binary: elsewhere
# the desktop entry or the bundle names the icon file. The include directory
# is what lets the .rc find the .ico by name.
#
# The resource is named GLFW_ICON because that is the name GLFW loads for its
# window class. Under any other name Explorer shows the icon on the file, but
# the window and its taskbar button get the generic application icon.
if(WIN32)
    configure_file(src/sweeppp.rc.in "${CMAKE_CURRENT_BINARY_DIR}/sweeppp.rc" @ONLY)
    target_sources(sweeppp-gui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sweeppp.rc")
    target_include_directories(sweeppp-gui PRIVATE "${PROJECT_SOURCE_DIR}/branding")

    # Linked for the GUI subsystem, so Windows does not put a console window
    # behind the application every time it is started. The entry point stays
    # main(): the GUI subsystem makes the linker look for WinMain instead, and
    # mainCRTStartup is the runtime's own entry for the one this tree has. The
    # other three binaries are typed at a shell and stay console subsystem.
    set_target_properties(sweeppp-gui PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(sweeppp-gui PRIVATE /ENTRY:mainCRTStartup)
    endif()

    # The .sweeps association script, beside the executable so it finds it
    # from a build tree as well as from the package. Configured because the
    # ProgId it registers is the channel's own.
    configure_file("${PROJECT_SOURCE_DIR}/packaging/windows/register-sweeps.cmd.in"
                   "${SWEEPPP_OUTPUT_DIR}/register-sweeps.cmd" @ONLY NEWLINE_STYLE CRLF)
endif()

# macOS shows a bare executable as its file name with a generic icon, and names
# the application menu after it. A bundle is what carries the icon, the real
# product name and a bundle identifier -- so the GUI is one, and only the GUI:
# the CLI tools are typed at a shell and stay plain executables.
#
# The product name -- "Sweep++", or "Sweep++ Nightly" -- names the bundle,
# because that is what a person reads in the Finder and the menu bar.
# Everything a machine parses -- the binary the other tools sit beside, the
# identifier, the package -- stays the app id.
if(APPLE)
    # The application's icon, and the document icon Info.plist names for
    # .sweeps files -- the same one for every channel, since a file is not a
    # nightly or a release.
    set(SWEEPPP_ICNS "${PROJECT_SOURCE_DIR}/branding/${SWEEPPP_APP_ID}.icns"
                     "${PROJECT_SOURCE_DIR}/branding/sweeps.icns")
    target_sources(sweeppp-gui PRIVATE ${SWEEPPP_ICNS})
    set_source_files_properties(${SWEEPPP_ICNS} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")

    # What Finder runs is macos-launcher.sh, staged beside the binary under
    # this name, which checks for the Homebrew libraries and then execs the
    # binary next to it. Without it a missing libglfw kills the process in
    # dyld, before any code of ours could say which library or how to get it.
    #
    # A name of its own rather than swapping the two, because CMake takes the
    # bundle directory's name from OUTPUT_NAME as well: renaming the binary to
    # make room renames the .app with it. CFBundleExecutable is just a file
    # name inside Contents/MacOS and is under no obligation to match either.
    set(SWEEPPP_BUNDLE_EXECUTABLE "sweeppp-launcher")
    # Names the bundle directory, the binary inside it and CFBundleName, which
    # CMake derives from OUTPUT_NAME and the launcher has to spell out for
    # itself.
    set(SWEEPPP_BUNDLE_NAME "${SWEEPPP_PRODUCT_NAME}")

    set_target_properties(sweeppp-gui PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "${SWEEPPP_BUNDLE_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${SWEEPPP_BUNDLE_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${SWEEPPP_BUNDLE_ID}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "${SWEEPPP_APP_ID}.icns"
        MACOSX_BUNDLE_COPYRIGHT "${SWEEPPP_COPYRIGHT}. GPL-3.0-or-later.")

    # Configured, because the script has to know the binary's name and the
    # product's, and both depend on the channel. Configured at configure time
    # and copied at build time, so that editing the template still reaches
    # the bundle on the next build.
    #
    # A target of its own rather than POST_BUILD on the GUI, because a
    # POST_BUILD command runs only when its target relinks -- so editing the
    # script and rebuilding left the previous copy in the bundle, silently.
    # Here the script is the dependency, which is what it actually is.
    # Written out rather than taken from $<TARGET_BUNDLE_CONTENT_DIR>, which is
    # not resolvable in an OUTPUT: the path has to be a literal for the build
    # system to track the file at all.
    set(SWEEPPP_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/macos-launcher.sh")
    configure_file(macos-launcher.sh.in "${SWEEPPP_LAUNCHER}" @ONLY)
    set(SWEEPPP_LAUNCHER_DEST
        "${SWEEPPP_OUTPUT_DIR}/${SWEEPPP_BUNDLE_NAME}.app/Contents/MacOS/${SWEEPPP_BUNDLE_EXECUTABLE}")
    add_custom_command(
        OUTPUT "${SWEEPPP_LAUNCHER_DEST}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SWEEPPP_LAUNCHER}" "${SWEEPPP_LAUNCHER_DEST}"
        COMMAND chmod +x "${SWEEPPP_LAUNCHER_DEST}"
        DEPENDS "${SWEEPPP_LAUNCHER}"
        COMMENT "Staging the launcher as the bundle's entry point"
        VERBATIM)
    # ALL so an ordinary build stages it, and after the GUI so the bundle it
    # is copied into exists.
    add_custom_target(sweeppp-gui-launcher ALL DEPENDS "${SWEEPPP_LAUNCHER_DEST}")
    add_dependencies(sweeppp-gui-launcher sweeppp-gui)
endif()

# The icon font's location, resolved at configure time. Absent when the font
# could not be fetched, in which case the UI falls back to text labels rather
# than failing to build.
if(SWEEPPP_ICON_FONT)
    target_compile_definitions(sweeppp-gui PRIVATE
        SWEEPPP_ICON_FONT_PATH="${SWEEPPP_ICON_FONT}")
endif()

target_link_libraries(sweeppp-gui PRIVATE
    nfd
    sweeppp::sweeppp
    sweeppp_imgui
    sweeppp_gl
    sweeppp_stb
    sweeppp_warnings)

# The update check: libcurl for the request, nlohmann for the answer, and the
# two URLs derived from the project's own homepage so the repository is named
# in exactly one place.
if(SWEEPPP_WITH_UPDATE_CHECK)
    string(REGEX REPLACE "^https://github\\.com/" "" SWEEPPP_REPO "${PROJECT_HOMEPAGE_URL}")
    target_compile_definitions(sweeppp-gui PRIVATE
        SWEEPPP_WITH_UPDATE_CHECK
        SWEEPPP_RELEASES_API="https://api.github.com/repos/${SWEEPPP_REPO}/releases/latest"
        SWEEPPP_RELEASES_PAGE="${PROJECT_HOMEPAGE_URL}/releases/latest")
    target_link_libraries(sweeppp-gui PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
endif()

# The snapshot's clipboard half, which has no portable form: GLFW's clipboard
# carries text only. Each platform's toolkit is already linked for the file
# dialog, so this adds a source and a framework rather than a dependency.
if(APPLE)
    target_sources(sweeppp-gui PRIVATE src/SnapshotPasteboard.m src/MacOpenDocument.m
        src/FileDialogSheet.m)
    target_link_libraries(sweeppp-gui PRIVATE "-framework AppKit"
        "-framework UniformTypeIdentifiers")
elseif(UNIX)
    # The same GTK nativefiledialog-extended's default backend already needs;
    # named here rather than inherited through it, because a build that
    # switched NFD to the portal backend would otherwise stop linking.
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    target_link_libraries(sweeppp-gui PRIVATE PkgConfig::GTK3)
endif()
