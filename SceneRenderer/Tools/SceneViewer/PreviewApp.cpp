#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <OpenGL/gl.h>

#include <argparse/argparse.hpp>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <xlocale.h>

import rstd.cppstd;
import rstd.log;
import sr.scene_wallpaper;
import sr.json;
import sr.utils;
import viewer.common;

using namespace std;

atomic<bool> renderCall(false);

extern "C" void SceneRendererSetLiveFrameCallback(
    void (*cb)(const uint8_t*, uint32_t, uint32_t, void*), void* userdata);
extern "C" void SceneRendererSetLiveMetalFrameCallback(
    void (*cb)(void*, uint32_t, uint32_t, void*), void* userdata);
extern "C" void* SceneRendererMacMetalDisplayCreate(GLFWwindow* window);
extern "C" void  SceneRendererMacMetalDisplayDestroy(void* handle);
extern "C" void  SceneRendererMacMetalDisplayDraw(void* handle, void* texture, uint32_t width,
                                                  uint32_t height, void (*presented)(void*),
                                                  void* userdata);

struct LiveFrameState {
    std::mutex           mutex;
    std::vector<uint8_t> rgba;
    uint32_t             width { 0 };
    uint32_t             height { 0 };
    bool                 dirty { false };
};

struct LiveMetalFrameState {
    std::mutex mutex;
    void*      texture { nullptr };
    uint32_t   width { 0 };
    uint32_t   height { 0 };
    bool       dirty { false };
};

void live_frame_callback(const uint8_t* rgba, uint32_t width, uint32_t height, void* userdata) {
    auto* state = static_cast<LiveFrameState*>(userdata);
    if (state == nullptr || rgba == nullptr || width == 0 || height == 0) return;
    const auto size = static_cast<std::size_t>(width) * height * 4;
    {
        std::scoped_lock lock(state->mutex);
        state->rgba.assign(rgba, rgba + size);
        state->width  = width;
        state->height = height;
        state->dirty  = true;
    }
    glfwPostEmptyEvent();
}

void live_metal_frame_callback(void* texture, uint32_t width, uint32_t height, void* userdata) {
    auto* state = static_cast<LiveMetalFrameState*>(userdata);
    if (state == nullptr || texture == nullptr || width == 0 || height == 0) return;
    {
        std::scoped_lock lock(state->mutex);
        state->texture = texture;
        state->width   = width;
        state->height  = height;
        state->dirty   = true;
    }
    glfwPostEmptyEvent();
}

void draw_live_metal_frame(void* display, LiveMetalFrameState& state) {
    void*    texture = nullptr;
    uint32_t width = 0, height = 0;
    {
        std::scoped_lock lock(state.mutex);
        if (! state.dirty) return;
        texture     = state.texture;
        width       = state.width;
        height      = state.height;
        state.dirty = false;
    }
    if (display != nullptr && texture != nullptr) {
        SceneRendererMacMetalDisplayDraw(display, texture, width, height, nullptr, nullptr);
    }
}

void draw_live_frame(GLFWwindow* window, LiveFrameState& state) {
    std::vector<uint8_t> rgba;
    uint32_t             width = 0, height = 0;
    {
        std::scoped_lock lock(state.mutex);
        if (! state.rgba.empty()) {
            rgba   = state.rgba;
            width  = state.width;
            height = state.height;
        }
        state.dirty = false;
    }

    glfwMakeContextCurrent(window);
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    if (fbw <= 0 || fbh <= 0) return;

    glViewport(0, 0, fbw, fbh);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (! rgba.empty() && width > 0 && height > 0) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(fbw), 0.0, static_cast<double>(fbh), -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glRasterPos2i(0, fbh);
        glPixelZoom(static_cast<float>(fbw) / static_cast<float>(width),
                    -static_cast<float>(fbh) / static_cast<float>(height));
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glDrawPixels(static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     rgba.data());
        glPixelZoom(1.0f, 1.0f);
    }

    glfwSwapBuffers(window);
}

struct UserData {
    sr::SceneWallpaper* psw { nullptr };
    bool                 mouse_position_locked { false };

    uint16_t width;
    uint16_t height;
};

extern "C" {
void framebuffer_size_callback(GLFWwindow* win, int width, int height) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data) return;
    if (width > 0) data->width = static_cast<uint16_t>(width);
    if (height > 0) data->height = static_cast<uint16_t>(height);
}

void mouse_button_callback(GLFWwindow* win, int button, int action, int /*mods*/) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw) return;
    // GLFW button numbering (0=left, 1=right, 2=middle) matches WE.
    if (action == GLFW_PRESS) data->psw->mouseButton(button, true);
    if (action == GLFW_RELEASE) data->psw->mouseButton(button, false);
}

void cursor_position_callback(GLFWwindow* win, double xpos, double ypos) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw || data->mouse_position_locked) return;
    data->psw->mouseInput(xpos / data->width, ypos / data->height);
}

void cursor_enter_callback(GLFWwindow* win, int entered) {
    UserData* data = static_cast<UserData*>(glfwGetWindowUserPointer(win));
    if (! data || ! data->psw || data->mouse_position_locked) return;
    data->psw->mouseEnter(entered != 0);
}
}

void updateCallback() {
    renderCall = true;
    glfwPostEmptyEvent();
}

bool parseDouble(std::string_view text, double& out) {
    if (text.empty()) return false;
    std::string value_text(text);
    char*       parsed_end = nullptr;
    static locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    errno = 0;
    const double value = c_locale != nullptr
                             ? strtod_l(value_text.c_str(), &parsed_end, c_locale)
                             : std::strtod(value_text.c_str(), &parsed_end);
    if (errno == ERANGE || parsed_end != value_text.data() + value_text.size() ||
        ! std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

std::optional<std::array<double, 2>> parseMousePosition(const std::string& value) {
    if (value.empty()) return std::nullopt;
    const auto comma = value.find(',');
    if (comma == std::string::npos) return std::nullopt;
    double x  = 0.0;
    double y  = 0.0;
    auto   xs = value.substr(0, comma);
    auto   ys = value.substr(comma + 1);
    if (! parseDouble(xs, x) || ! parseDouble(ys, y)) return std::nullopt;
    return std::array { std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0) };
}

const char* envValue(const char* primary) {
    const char* value = std::getenv(primary);
    if (value != nullptr && value[0] != '\0') return value;
    return nullptr;
}

bool envEnabled(const char* primary) {
    const char* value = envValue(primary);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

int main(int argc, char** argv) {
    static rstd::log::EnvLogger _logger;
    rstd::log::set_logger(_logger);
    rstd::log::set_max_level(_logger.filter());

    argparse::ArgumentParser program("SceneViewer");
    viewer::setAndParseArg(program, argc, argv);
    auto [w_width, w_height] = program.get<viewer::Resolution>(viewer::OPT_RESOLUTION);

    glfwSetErrorCallback([](int code, const char* desc) {
        std::cerr << "GLFW error " << code << ": " << (desc ? desc : "(null)") << "\n";
    });
    // MoltenVK reads these at startup. Keep them overrideable from the shell,
    // but choose the conservative presentation path for a standalone Cocoa
    // debug window. This helps avoid CAMetalLayer transactions lingering until
    // the window is closed.
    setenv("MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER", "1", /*overwrite=*/0);
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "1", /*overwrite=*/0);
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif
    if (! glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }
    if (! glfwVulkanSupported()) {
        std::cerr << "GLFW cannot find a Vulkan loader/runtime\n";
        glfwTerminate();
        return -1;
    }
    GLFWwindow* surface_window = nullptr;
    GLFWwindow* window         = nullptr;
    const char* direct_present = envValue("SCENERENDERER_MACOS_DIRECT_PRESENT");
    const char* cpu_fallback   = envValue("SCENERENDERER_MACOS_CPU_FALLBACK");
    const bool  use_direct_present = direct_present && direct_present[0] == '1';
    const bool  use_cpu_display_fallback =
        ! use_direct_present && cpu_fallback && cpu_fallback[0] != '\0' &&
        cpu_fallback[0] != '0';
    const bool use_metal_display_fallback = ! use_direct_present && ! use_cpu_display_fallback;
    const bool use_display_fallback = use_cpu_display_fallback || use_metal_display_fallback;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
    // Bulk-scan path: SCENERENDERER_HEADLESS=1 hides the window so a scan loop over
    // hundreds of pkgs doesn't spam the desktop. Compile/render still
    // runs against the offscreen surface — stderr captures shader errors.
    if (envEnabled("SCENERENDERER_HEADLESS")) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }
    if (use_display_fallback) {
        if (use_cpu_display_fallback) {
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            surface_window = glfwCreateWindow(w_width,
                                              w_height,
                                              "SceneRenderer Vulkan Producer",
                                              nullptr,
                                              nullptr);
            if (surface_window == nullptr) {
                std::cerr << "Failed to create hidden Vulkan GLFW window\n";
                glfwTerminate();
                return -1;
            }
        }

        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API,
                       use_cpu_display_fallback ? GLFW_OPENGL_API : GLFW_NO_API);
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
        window = glfwCreateWindow(w_width,
                                  w_height,
                                  use_cpu_display_fallback
                                      ? "SceneRenderer " __DATE__ " " __TIME__ " CPU Fallback"
                                      : "SceneRenderer " __DATE__ " " __TIME__
                                                   " Metal Fallback",
                                  nullptr,
                                  nullptr);
        if (window == nullptr) {
            std::cerr << "Failed to create display fallback GLFW window\n";
            if (surface_window != nullptr) glfwDestroyWindow(surface_window);
            glfwTerminate();
            return -1;
        }
        if (use_cpu_display_fallback) {
            glfwMakeContextCurrent(window);
            glfwSwapInterval(1);
        }
    } else {
        surface_window = glfwCreateWindow(w_width,
                                          w_height,
                                          "SceneRenderer " __DATE__ " " __TIME__,
                                          nullptr,
                                          nullptr);
        window = surface_window;
    }
    if (window == nullptr) {
        std::cerr << "Failed to create GLFW window\n";
        if (surface_window && surface_window != window) glfwDestroyWindow(surface_window);
        glfwTerminate();
        return -1;
    }
    glfwPollEvents();

    UserData data;
    data.width  = w_width;
    data.height = w_height;

    int fb_width = 0, fb_height = 0;
    glfwGetFramebufferSize(surface_window != nullptr ? surface_window : window, &fb_width, &fb_height);
    if (fb_width <= 0 || fb_height <= 0) {
        fb_width  = static_cast<int>(w_width);
        fb_height = static_cast<int>(w_height);
    }

    sr::RenderInitInfo info;
    info.enable_valid_layer = program.get<bool>(viewer::OPT_VALID_LAYER);
    info.width              = static_cast<uint32_t>(fb_width);
    info.height             = static_cast<uint32_t>(fb_height);
    info.msaa_samples       = program.get<uint32_t>(viewer::OPT_MSAA);
    info.redraw_callback    = updateCallback;
    info.offscreen = use_metal_display_fallback;

    auto& sf_info = info.surface_info;
    if (! info.offscreen) {
        uint32_t glfwExtCount = 0;
        auto     exts         = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        if (exts == nullptr || glfwExtCount == 0) {
            std::cerr << "GLFW did not report Vulkan surface instance extensions\n";
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }
        for (uint32_t i = 0; i < glfwExtCount; i++) {
            sf_info.instanceExts.emplace_back(exts[i]);
        }

        sf_info.createSurfaceOp = [surface_window](VkInstance inst, VkSurfaceKHR* surface) {
            return glfwCreateWindowSurface(inst, surface_window, NULL, surface);
        };
    }

    auto* psw = new sr::SceneWallpaper();
    data.psw  = psw;

    psw->init();

    sr::SceneWallpaperConfig config;
    config.assets_dir      = program.get<std::string>(viewer::ARG_ASSETS);
    config.source_pkg_path = program.get<std::string>(viewer::ARG_SCENE);
    config.graphviz        = program.get<bool>(viewer::OPT_GRAPHVIZ);
    config.fps             = static_cast<uint32_t>(program.get<int32_t>(viewer::OPT_FPS));

    std::string cache_path = program.get<std::string>(viewer::OPT_CACHE_PATH);
    if (cache_path.empty()) cache_path = sr::platform::GetCachePath("SceneRenderer");
    config.cache_dir = std::move(cache_path);

    LiveFrameState      live_frame_state;
    LiveMetalFrameState live_metal_frame_state;
    void*               metal_display = nullptr;
    if (use_cpu_display_fallback) {
        SceneRendererSetLiveFrameCallback(live_frame_callback, &live_frame_state);
    } else if (use_metal_display_fallback) {
        metal_display = SceneRendererMacMetalDisplayCreate(window);
        if (metal_display == nullptr) {
            std::cerr << "Failed to create Metal display fallback\n";
            delete psw;
            if (surface_window && surface_window != window) glfwDestroyWindow(surface_window);
            glfwDestroyWindow(window);
            glfwTerminate();
            return -1;
        }
        SceneRendererSetLiveMetalFrameCallback(live_metal_frame_callback,
                                               &live_metal_frame_state);
    }

    // Apply --user-properties FILE before the scene loads so the first
    // frame already reflects the user's edits. Mirrors the daemon path
    // (Init.user_properties): JSON object whose values can be strings,
    // numbers, or booleans.
    if (auto up_path = program.get<std::string>(viewer::OPT_USER_PROPS); ! up_path.empty()) {
        std::ifstream is(up_path);
        if (! is) {
            std::cerr << "--user-properties: cannot open '" << up_path << "'\n";
            return 1;
        }
        std::stringstream ss;
        ss << is.rdbuf();
        auto parsed = sr::ParseJson(ss.str(), { .allow_comments = true });
        if (parsed.is_err()) {
            std::cerr << "--user-properties: '" << up_path << "' is not a JSON object\n";
            return 1;
        }
        auto value = parsed.unwrap();
        if (! value.is_object()) {
            std::cerr << "--user-properties: '" << up_path << "' is not a JSON object\n";
            return 1;
        }
        auto object = value.as_object();
        (*object)->iter().for_each([&](auto entry) {
            auto [key, property] = entry;
            config.user_properties.insert(
                ::alloc::string::String::make(key->as_str()), property->clone());
        });
    }

    psw->configure(std::move(config));
    psw->initVulkan(std::move(info));

    glfwSetWindowUserPointer(window, &data);

    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);
    glfwSetCursorEnterCallback(window, cursor_enter_callback);

    auto locked_mouse = parseMousePosition(program.get<std::string>(viewer::OPT_MOUSE_POS));
    data.mouse_position_locked = locked_mouse.has_value();
    auto apply_locked_mouse    = [&]() {
        if (! locked_mouse) return;
        psw->mouseEnter(true);
        psw->mouseInput((*locked_mouse)[0], (*locked_mouse)[1]);
        glfwSetCursorPos(window, (*locked_mouse)[0] * w_width, (*locked_mouse)[1] * w_height);
    };
    apply_locked_mouse();

    // Bulk-scan path: SCENERENDERER_COMPILE_ONLY=N waits N seconds after scene load
    // to let the async shader-compile pass drain, then exits. Skips the
    // render loop so no swapchain present is required (which would deadlock
    // with a hidden window). Use together with SCENERENDERER_HEADLESS=1.
    if (const char* co = envValue("SCENERENDERER_COMPILE_ONLY");
        co && co[0] != '\0') {
        int seconds = std::atoi(co);
        if (seconds <= 0) seconds = 2;
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    } else {
        const double wait_seconds = 1.0 / std::max(15, program.get<int32_t>(viewer::OPT_FPS));
        while (! glfwWindowShouldClose(window)) {
            // Letting the main thread enter GLFW's wait path gives AppKit /
            // CoreAnimation a chance to commit CAMetalLayer presents. A tight
            // glfwPollEvents loop can leave the drawable visible only when a
            // final window-close transaction is flushed.
            glfwWaitEventsTimeout(wait_seconds);
            apply_locked_mouse();
            if (use_cpu_display_fallback) draw_live_frame(window, live_frame_state);
            if (use_metal_display_fallback)
                draw_live_metal_frame(metal_display, live_metal_frame_state);
        }
    }
    if (use_cpu_display_fallback) SceneRendererSetLiveFrameCallback(nullptr, nullptr);
    if (use_metal_display_fallback) {
        SceneRendererSetLiveMetalFrameCallback(nullptr, nullptr);
        SceneRendererMacMetalDisplayDestroy(metal_display);
    }
    delete psw;
    glfwDestroyWindow(window);
    if (surface_window && surface_window != window) glfwDestroyWindow(surface_window);
    glfwTerminate();
    return 0;
}
