// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AppWindow.hpp"

#include "Icons.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <sweeppp/core/Log.hpp>
#include <sweeppp/core/Paths.hpp>
#include <sweeppp/core/Version.hpp>
#include <sweeppp/plugin/PluginHost.hpp>
#include <sweeppp_gl.h>
#include <utility>

namespace sweeppp::ui {
namespace {

void glfwErrorCallback(int code, const char* description) {
    std::fputs(std::format("[glfw] error {}: {}\n", code, description).c_str(), stderr);
}

const char* glString(GLenum name) {
    const GLubyte* value = glGetString(name);
    return value != nullptr ? reinterpret_cast<const char*>(value) : "<unknown>";
}

/// Hands the plugin host this process's ImGui, so a plugin's own copy can
/// adopt it.
///
/// A plugin links its own ImGui and shares ours -- context and allocators --
/// which is ImGui's documented arrangement for a dynamically loaded module.
/// The sizes are the dangerous part and the reason they are carried at all:
/// `sweeppp_imconfig.h` makes `ImDrawIdx` and `ImWchar` 32-bit, so a plugin
/// built without that header has a different `ImDrawVert` and corrupts every
/// draw list it touches, silently and not at the call that did it. The plugin
/// side compares these through `ImGui::DebugCheckVersionAndDataLayout` before
/// it is allowed to draw.
///
/// The allocators go over first for the same class of reason: memory allocated
/// by one copy's allocator and freed by the other's is a heap corruption that
/// shows up once a week somewhere unrelated.
void publishImGuiBinding() {
    ImGuiMemAllocFunc allocFn = nullptr;
    ImGuiMemFreeFunc freeFn = nullptr;
    void* allocUser = nullptr;
    ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &allocUser);

    sweeppp_imgui_binding_t binding{};
    binding.struct_size = sizeof(binding);
    binding.imgui_context = ImGui::GetCurrentContext();
    binding.implot_context = ImPlot::GetCurrentContext();
    binding.alloc_fn = reinterpret_cast<void* (*)(std::size_t, void*)>(allocFn);
    binding.free_fn = reinterpret_cast<void (*)(void*, void*)>(freeFn);
    binding.allocator_user = allocUser;
    binding.imgui_version = {IMGUI_VERSION, std::string_view(IMGUI_VERSION).size()};
    binding.implot_version = {IMPLOT_VERSION, std::string_view(IMPLOT_VERSION).size()};
    binding.size_of_io = sizeof(ImGuiIO);
    binding.size_of_style = sizeof(ImGuiStyle);
    binding.size_of_vec2 = sizeof(ImVec2);
    binding.size_of_vec4 = sizeof(ImVec4);
    binding.size_of_draw_vert = sizeof(ImDrawVert);
    binding.size_of_draw_idx = sizeof(ImDrawIdx);

    PluginManager::instance().setImGuiBinding(binding);
}

/// A candidate UI font.
///
/// `alreadyHeavy` says the face is a Medium/Semibold cut. A Regular face needs
/// its coverage boosted to look right (see below); a Medium one does not, and
/// boosting it too would smear the counters shut.
struct FontCandidate {
    const char* path;
    bool alreadyHeavy;
};

/// System UI fonts, most preferred first.
///
/// Not vendored: a font is a large binary with its own licence terms, and
/// every desktop already ships good UI faces. ImGui's built-in ProggyClean is
/// a 13 px bitmap font -- legible, but it makes the whole application look
/// like a terminal. Falling back to it is still correct, just plainer.
///
/// Medium weights are preferred over Regular throughout. An instrument panel
/// is read at a glance and often in a bright room, and the platform Regular
/// weights are tuned for text engines that apply stem darkening -- which
/// ImGui's rasteriser does not.
constexpr std::array<FontCandidate, 16> kFontCandidates{{
#if defined(__APPLE__)
    // Static cuts, present when the platform design-resources package is
    // installed. Genuinely heavier outlines, not a synthesised approximation.
    {"/Library/Fonts/SF-Pro-Text-Medium.otf", true},
    {"/Library/Fonts/SF-Pro-Display-Medium.otf", true},
    {"/Library/Fonts/SF-Compact-Text-Medium.otf", true},
    {"/System/Library/Fonts/Supplemental/HelveticaNeue-Medium.otf", true},
    // The system variable face. stb_truetype has no variable-axis support, so
    // this renders at its default Regular instance and needs the boost.
    {"/System/Library/Fonts/SFNS.ttf", false},
    {"/System/Library/Fonts/Helvetica.ttc", false},
    {"/System/Library/Fonts/Supplemental/Arial.ttf", false},
#elif defined(_WIN32)
    {"C:\\Windows\\Fonts\\segoeuisb.ttf", true},
    {"C:\\Windows\\Fonts\\segoeui.ttf", false},
    {"C:\\Windows\\Fonts\\tahoma.ttf", false},
    {"C:\\Windows\\Fonts\\arial.ttf", false},
#else
    // Regular, and no Bold above it. DejaVu ships no Medium, so the heavier
    // cut available here is a full Bold -- and picking it made every label in
    // the application read as bold on Linux and nowhere else. The coverage
    // boost below is the right amount of darkening for a Regular; a Bold face
    // is a different typeface, not a darker rendering of this one.
    {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", false},
    {"/usr/share/fonts/TTF/DejaVuSans.ttf", false},
    {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", false},
    {"/usr/share/fonts/noto/NotoSans-Regular.ttf", false},
#endif
}};

} // namespace

void AppWindow::loadFonts(float baseSize) {
    ImGuiIO& io = ImGui::GetIO();

    // The size is in logical points, and it is NOT pre-multiplied by the DPI
    // scale. ImGui 1.92's dynamic atlas derives the rasterisation density from
    // the viewport's framebuffer scale by itself, so doing it here as well
    // would either double-scale the layout or, with a compensating
    // FontScaleMain, rasterise at the wrong size and come out soft.
    //
    // The operator's own size arrives the same way, unmultiplied, and where
    // the display needs a scale of its own -- Windows at 150% -- that is
    // applied once as style.FontSizeBase alongside every other metric, not
    // baked into the atlas here.

    for (const FontCandidate& candidate : kFontCandidates) {
        if (candidate.path == nullptr) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate.path, ec)) {
            continue;
        }

        ImFontConfig config;
        // Boosts the rasterised glyph coverage. This multiplies alpha rather
        // than widening outlines, so it recovers the weight a text engine's
        // stem darkening would have added -- but it cannot turn Light into
        // Bold, which is why a real Medium face is preferred above.
        //
        // How much a face needs is a property of the face, and the operator's
        // preference is a factor over that rather than a figure of its own:
        // the platforms disagree about how heavy their text looks, and only
        // the person looking at it can settle that.
        const float boost = (candidate.alreadyHeavy ? 1.1F : 1.6F) * m_fontWeight;
        config.RasterizerMultiply = boost;

        // AddFontFromFileTTF returns null on a format stb_truetype cannot
        // read (some .otf CFF variants), so an unusable candidate simply falls
        // through to the next.
        if (io.Fonts->AddFontFromFileTTF(candidate.path, baseSize, &config) != nullptr) {
            m_fontDescription = std::format(
                "{} at {:.0f}pt ({}, coverage {:.2f}x)",
                std::filesystem::path(candidate.path).filename().string(),
                static_cast<double>(baseSize), candidate.alreadyHeavy ? "medium" : "regular",
                static_cast<double>(boost));
            loadIconFont(baseSize);
            return;
        }
    }

    io.Fonts->AddFontDefault();
    m_fontDescription = "built-in (no system UI font found)";
    loadIconFont(baseSize);
}

void AppWindow::loadIconFont(float baseSize) {
    ImGuiIO& io = ImGui::GetIO();

    // Beside the executable first, in the build tree second.
    //
    // SWEEPPP_ICON_FONT_PATH is an absolute path into the build directory,
    // fixed at configure time, so it resolves on the machine that produced the
    // binary and on no other. On its own it means every packaged build falls
    // back to text labels -- and silently, which is how it went unnoticed.
    // resources/fonts/ is where the packaging step puts the font, and Paths
    // resolves that relative to the executable, so it travels with the build.
    std::error_code ec;
    std::filesystem::path font;
    const std::filesystem::path packaged =
        Paths::instance().resourcesDir() / "fonts" / "materialdesignicons-webfont.ttf";
    if (std::filesystem::is_regular_file(packaged, ec)) {
        font = packaged;
    }
#ifdef SWEEPPP_ICON_FONT_PATH
    if (font.empty() && std::filesystem::is_regular_file(SWEEPPP_ICON_FONT_PATH, ec)) {
        font = SWEEPPP_ICON_FONT_PATH;
    }
#endif

    if (font.empty()) {
        // Said out loud: the UI is still usable with text labels, but "the
        // icons are missing" should not be something an operator has to work
        // out for themselves.
        logWarn("ui", "icon font not found at {}; falling back to text labels", packaged.string());
        return;
    }

    ImFontConfig config;
    // Merged into the text font rather than added as a second one, so an icon
    // and a label can sit in the same string and the caller never has to push
    // a font to draw a button.
    config.MergeMode = true;

    // Icons are drawn as glyphs on a text baseline, and at the same nominal
    // size they read noticeably larger than the letters beside them. Slightly
    // smaller, nudged down, puts their optical centre on the text's.
    config.GlyphMinAdvanceX = baseSize;
    config.GlyphOffset = ImVec2(0.0F, 1.0F);

    // A range, not a list: 1.92 rasterises on demand, so covering the whole
    // block costs nothing until a glyph is actually used.
    static const ImWchar kRanges[] = {static_cast<ImWchar>(icon::kFirstCodepoint),
                                      static_cast<ImWchar>(icon::kLastCodepoint), 0};
    config.GlyphRanges = kRanges;

    m_iconsAvailable =
        io.Fonts->AddFontFromFileTTF(font.string().c_str(), baseSize * 0.92F, &config) != nullptr;
    icon::setAvailable(m_iconsAvailable);
}

AppWindow::~AppWindow() {
    destroy();
}

std::expected<void, std::string> AppWindow::create(const Options& options) {
    glfwSetErrorCallback(&glfwErrorCallback);

    if (glfwInit() != GLFW_TRUE) {
        return std::unexpected("failed to initialise GLFW");
    }
    m_glfwInitialised = true;

    // 4.1 core is the ceiling on macOS and the floor everywhere else, so it is
    // the one profile that runs unmodified on all three platforms.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    // How the window finds its icon on Linux: both X11 and Wayland identify an
    // application by this string and look for the desktop entry of the same
    // name -- <appId>.desktop, which names the installed icon. Leave it unset
    // and a correctly installed icon still never reaches the window, because
    // nothing connects the two. Windows takes its icon from the executable's
    // resource instead, and macOS windows have no icon at all.
    //
    // The channel's id rather than a fixed name, so a nightly's windows group
    // under the nightly's entry and icon and not the release's.
    const std::string windowClass(appId());
#ifdef GLFW_X11_CLASS_NAME
    glfwWindowHintString(GLFW_X11_CLASS_NAME, windowClass.c_str());
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, windowClass.c_str());
#endif
#ifdef GLFW_WAYLAND_APP_ID
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, windowClass.c_str());
#endif

    m_window =
        glfwCreateWindow(options.width, options.height, options.title.c_str(), nullptr, nullptr);
    if (m_window == nullptr) {
        destroy();
        return std::unexpected("failed to create an OpenGL 4.1 core window");
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetDropCallback(m_window, [](GLFWwindow* window, int count, const char** paths) {
        auto* self = static_cast<AppWindow*>(glfwGetWindowUserPointer(window));
        if (self == nullptr || !self->m_fileDropHandler) {
            return;
        }
        for (int i = 0; i < count; ++i) {
            self->m_fileDropHandler(std::filesystem::path(paths[i]));
        }
    });

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(options.vsync ? 1 : 0);

    if (const int missing =
            sweeppp_gl_load(reinterpret_cast<SweepppGlGetProcAddress>(glfwGetProcAddress));
        missing != 0) {
        const char* first = sweeppp_gl_first_missing();
        auto message = std::format("this driver is missing {} OpenGL 4.1 entry point(s), "
                                   "starting with {}",
                                   missing, first != nullptr ? first : "<unknown>");
        destroy();
        return std::unexpected(std::move(message));
    }

    m_rendererDescription =
        std::format("{} | {} | GL {} | GLSL {}", glString(GL_VENDOR), glString(GL_RENDERER),
                    glString(GL_VERSION), glString(GL_SHADING_LANGUAGE_VERSION));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    m_implotInitialised = true;

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // layout persistence is owned by the profile system

    // Content scale, not framebuffer size: GLFW_SCALE_TO_MONITOR means the
    // window is already sized in points, and this is the factor between those
    // and real pixels.
    float scaleX = 1.0F;
    float scaleY = 1.0F;
    glfwGetWindowContentScale(m_window, &scaleX, &scaleY);
    m_dpiScale = std::max(scaleX, 1.0F);

    // What the interface still has to be scaled by after ImGui's own handling
    // of the framebuffer; see displayUiScale().
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(m_window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(m_window, &framebufferWidth, &framebufferHeight);
    const float framebufferScale =
        windowWidth > 0 ? static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth)
                        : 1.0F;
    m_displayUiScale = std::max(m_dpiScale / std::max(framebufferScale, 0.01F), 1.0F);

    m_fontBaseSize = options.fontSize;
    m_fontWeight = options.fontWeight;
    loadFonts(m_fontBaseSize);

    if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true)) {
        destroy();
        return std::unexpected("failed to initialise the ImGui GLFW backend");
    }
    if (!ImGui_ImplOpenGL3_Init("#version 410 core")) {
        destroy();
        return std::unexpected("failed to initialise the ImGui OpenGL backend");
    }
    m_imguiInitialised = true;

    // Before AppState::initialise, which is where plugins are discovered: a UI
    // facet registering into a host that has not published its context yet
    // would be listed as "this build has no user interface".
    publishImGuiBinding();

    return {};
}

bool AppWindow::running() const noexcept {
    return m_window != nullptr && glfwWindowShouldClose(m_window) == 0;
}

void AppWindow::setFileDropHandler(std::function<void(const std::filesystem::path&)> handler) {
    m_fileDropHandler = std::move(handler);
}

void AppWindow::requestFrameCapture(std::function<void()> capture) {
    m_frameCapture = std::move(capture);
}

void AppWindow::setFontWeight(float weight) {
    const float wanted = std::clamp(weight, 0.1F, 4.0F);
    if (wanted == m_fontWeight) {
        return;
    }

    m_fontWeight = wanted;
    m_fontsStale = true;
}

void AppWindow::beginFrame() {
    glfwPollEvents();

    // Here, and not where the preference was changed: Clear() frees the fonts
    // and the atlas texture, and the frame that asked for it was still drawing
    // with both. Between the swap and the next NewFrame nothing holds either.
    if (m_fontsStale) {
        m_fontsStale = false;
        ImGui::GetIO().Fonts->Clear();
        loadFonts(m_fontBaseSize);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void AppWindow::endFrame() {
    ImGui::Render();

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    glViewport(0, 0, width, height);

    const ImVec4 clear = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    glClearColor(clear.x, clear.y, clear.z, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Between the draw and the swap. Taken out of the member first, so a
    // capture that asks for another one does not run twice in a frame.
    if (m_frameCapture) {
        const std::function<void()> capture = std::exchange(m_frameCapture, nullptr);
        capture();
    }

    glfwSwapBuffers(m_window);
}

void AppWindow::destroy() noexcept {
    if (m_imguiInitialised) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        m_imguiInitialised = false;
    }
    if (m_implotInitialised) {
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        m_implotInitialised = false;
    }
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    if (m_glfwInitialised) {
        glfwTerminate();
        m_glfwInitialised = false;
    }
}

} // namespace sweeppp::ui
