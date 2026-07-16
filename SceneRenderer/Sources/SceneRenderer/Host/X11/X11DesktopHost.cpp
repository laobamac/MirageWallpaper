#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "X11DesktopHost.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <span>
#include <thread>
#include <unistd.h>
#include <sys/select.h>

namespace sr::host
{
namespace
{

struct MotifWmHints {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long          input_mode;
    unsigned long status;
};

constexpr unsigned long MWM_HINTS_DECORATIONS = 1u << 1u;
constexpr unsigned long NET_WM_DESKTOP_ALL = 0xFFFFFFFFu;

struct X11DesktopHost {
    DesktopCallbacks callbacks {};
    Display*         display { nullptr };
    int              screen { 0 };
    Window           root { 0 };
    Window           window { 0 };
    Atom             wm_delete_window { None };
    int              wake_pipe[2] { -1, -1 };
    std::uint32_t    width { 0 };
    std::uint32_t    height { 0 };
    std::uint32_t    input_hz { 60 };
    unsigned int     last_buttons { 0 };
    bool             mouse_inside { false };
    bool             sent_enter { false };
    std::atomic<bool> stop { false };
};

bool EnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool IsWaylandSession() {
    const char* session_type = std::getenv("XDG_SESSION_TYPE");
    return session_type != nullptr && std::strcmp(session_type, "wayland") == 0;
}

Atom InternAtom(Display* display, const char* name) {
    return XInternAtom(display, name, False);
}

void SetAtomListProperty(Display* display, Window window, const char* property_name,
                         std::span<const Atom> atoms) {
    const Atom property = InternAtom(display, property_name);
    XChangeProperty(display,
                    window,
                    property,
                    XA_ATOM,
                    32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(atoms.data()),
                    static_cast<int>(atoms.size()));
}

void SetDesktopWindowHints(X11DesktopHost& host) {
    const Atom window_type_desktop = InternAtom(host.display, "_NET_WM_WINDOW_TYPE_DESKTOP");
    SetAtomListProperty(host.display, host.window, "_NET_WM_WINDOW_TYPE", { &window_type_desktop, 1 });

    std::array states {
        InternAtom(host.display, "_NET_WM_STATE_BELOW"),
        InternAtom(host.display, "_NET_WM_STATE_STICKY"),
        InternAtom(host.display, "_NET_WM_STATE_SKIP_TASKBAR"),
        InternAtom(host.display, "_NET_WM_STATE_SKIP_PAGER"),
    };
    SetAtomListProperty(host.display, host.window, "_NET_WM_STATE", states);

    const Atom desktop_atom = InternAtom(host.display, "_NET_WM_DESKTOP");
    const unsigned long desktop = NET_WM_DESKTOP_ALL;
    XChangeProperty(host.display,
                    host.window,
                    desktop_atom,
                    XA_CARDINAL,
                    32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&desktop),
                    1);

    const Atom motif_hints_atom = InternAtom(host.display, "_MOTIF_WM_HINTS");
    MotifWmHints motif_hints {
        .flags       = MWM_HINTS_DECORATIONS,
        .functions   = 0,
        .decorations = 0,
        .input_mode  = 0,
        .status      = 0,
    };
    XChangeProperty(host.display,
                    host.window,
                    motif_hints_atom,
                    motif_hints_atom,
                    32,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&motif_hints),
                    5);

    XWMHints* wm_hints = XAllocWMHints();
    if (wm_hints != nullptr) {
        wm_hints->flags = InputHint;
        wm_hints->input = False;
        XSetWMHints(host.display, host.window, wm_hints);
        XFree(wm_hints);
    }
}

bool SetNonBlockingCloseOnExec(int fd) {
    if (fd < 0) return false;
    const int status_flags = fcntl(fd, F_GETFL, 0);
    if (status_flags < 0 || fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) return false;

    const int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0) return false;
    return true;
}

bool CreateWakePipe(int (&fds)[2]) {
    if (pipe(fds) != 0) return false;
    if (SetNonBlockingCloseOnExec(fds[0]) && SetNonBlockingCloseOnExec(fds[1])) return true;

    for (int& fd : fds) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
    return false;
}

void WakePipe(int fd) {
    if (fd < 0) return;
    const std::uint8_t byte = 1;
    const ssize_t      written = write(fd, &byte, sizeof(byte));
    (void)written;
}

void DrainPipe(int fd) {
    if (fd < 0) return;
    std::uint8_t buffer[64];
    for (;;) {
        const ssize_t bytes = read(fd, buffer, sizeof(buffer));
        if (bytes > 0) continue;
        if (bytes == 0) return;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        return;
    }
}

void EmitMouseEnter(X11DesktopHost& host, bool entered) {
    if (host.sent_enter && host.mouse_inside == entered) return;
    host.sent_enter   = true;
    host.mouse_inside = entered;
    if (host.callbacks.mouse_enter != nullptr) {
        host.callbacks.mouse_enter(entered ? 1 : 0, host.callbacks.userdata);
    }
}

void PollInput(X11DesktopHost& host) {
    Window root_return = 0;
    Window child_return = 0;
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;
    if (! XQueryPointer(host.display,
                        host.root,
                        &root_return,
                        &child_return,
                        &root_x,
                        &root_y,
                        &win_x,
                        &win_y,
                        &mask)) {
        EmitMouseEnter(host, false);
        return;
    }

    const bool inside = root_x >= 0 && root_y >= 0 &&
                        root_x < static_cast<int>(host.width) &&
                        root_y < static_cast<int>(host.height);
    EmitMouseEnter(host, inside);
    if (inside && host.callbacks.mouse_move != nullptr && host.width > 0 && host.height > 0) {
        const double x = std::clamp(static_cast<double>(root_x) / host.width, 0.0, 1.0);
        const double y = std::clamp(static_cast<double>(root_y) / host.height, 0.0, 1.0);
        host.callbacks.mouse_move(x, y, host.callbacks.userdata);
    }

    const std::array button_masks { Button1Mask, Button3Mask, Button2Mask };
    for (int button = 0; button < static_cast<int>(button_masks.size()); ++button) {
        const bool was_down = (host.last_buttons & button_masks[button]) != 0;
        const bool is_down  = (mask & button_masks[button]) != 0;
        if (was_down == is_down) continue;
        if (host.callbacks.mouse_button != nullptr) {
            host.callbacks.mouse_button(button, is_down ? 1 : 0, host.callbacks.userdata);
        }
    }
    host.last_buttons = mask;
}

void HandleEvent(X11DesktopHost& host, XEvent& event) {
    switch (event.type) {
    case ConfigureNotify:
        if (event.xconfigure.width > 0) {
            host.width = static_cast<std::uint32_t>(event.xconfigure.width);
        }
        if (event.xconfigure.height > 0) {
            host.height = static_cast<std::uint32_t>(event.xconfigure.height);
        }
        break;
    case ClientMessage:
        if (static_cast<Atom>(event.xclient.data.l[0]) == host.wm_delete_window) {
            host.stop.store(true);
        }
        break;
    case DestroyNotify:
        host.stop.store(true);
        break;
    default:
        break;
    }
}

void CloseHost(X11DesktopHost* host) {
    if (host == nullptr) return;
    if (host->display != nullptr) {
        if (host->window != 0) {
            XDestroyWindow(host->display, host->window);
            host->window = 0;
        }
        XCloseDisplay(host->display);
        host->display = nullptr;
    }
    for (int& fd : host->wake_pipe) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
    delete host;
}

} // namespace

void* DesktopCreate(const DesktopConfig* config, DesktopCallbacks callbacks) {
    if (IsWaylandSession() && ! EnvEnabled("SCENERENDERER_FORCE_X11")) {
        std::cerr << "SceneWallpaper Linux MVP requires an X11 session. "
                     "Wayland wallpaper support is compositor-specific and is not supported yet.\n";
        return nullptr;
    }
    if (std::getenv("DISPLAY") == nullptr) {
        std::cerr << "SceneWallpaper X11 host requires DISPLAY to be set.\n";
        return nullptr;
    }

    XInitThreads();
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cerr << "Failed to open X11 display.\n";
        return nullptr;
    }

    auto* host = new X11DesktopHost();
    host->callbacks = callbacks;
    host->display   = display;
    host->screen    = DefaultScreen(display);
    host->root      = RootWindow(display, host->screen);
    host->width     = static_cast<std::uint32_t>(DisplayWidth(display, host->screen));
    host->height    = static_cast<std::uint32_t>(DisplayHeight(display, host->screen));
    host->input_hz  = config != nullptr && config->input_hz > 0 ? config->input_hz : 60u;

    if (config != nullptr && config->screen_index != 0) {
        std::cerr << "SceneWallpaper X11 host currently uses the default X screen; "
                     "--screen is ignored on Linux MVP.\n";
    }

    XSetWindowAttributes attrs {};
    attrs.background_pixel = BlackPixel(display, host->screen);
    attrs.border_pixel     = BlackPixel(display, host->screen);
    attrs.event_mask       = StructureNotifyMask | PropertyChangeMask | ExposureMask;

    host->window = XCreateWindow(display,
                                 host->root,
                                 0,
                                 0,
                                 host->width,
                                 host->height,
                                 0,
                                 CopyFromParent,
                                 InputOutput,
                                 CopyFromParent,
                                 CWBackPixel | CWBorderPixel | CWEventMask,
                                 &attrs);
    if (host->window == 0) {
        std::cerr << "Failed to create X11 wallpaper window.\n";
        CloseHost(host);
        return nullptr;
    }

    const char* title = config != nullptr && config->title != nullptr && config->title[0] != '\0'
                            ? config->title
                            : "SceneRenderer Wallpaper";
    XStoreName(display, host->window, title);
    SetDesktopWindowHints(*host);

    host->wm_delete_window = InternAtom(display, "WM_DELETE_WINDOW");
    XSetWMProtocols(display, host->window, &host->wm_delete_window, 1);

    XMapWindow(display, host->window);
    XLowerWindow(display, host->window);
    XFlush(display);

    if (! CreateWakePipe(host->wake_pipe)) {
        std::cerr << "Failed to create X11 wake pipe: " << std::strerror(errno) << "\n";
        CloseHost(host);
        return nullptr;
    }

    PollInput(*host);
    return host;
}

void DesktopDestroy(void* handle) { CloseHost(static_cast<X11DesktopHost*>(handle)); }

int DesktopRun(void* handle) {
    auto* host = static_cast<X11DesktopHost*>(handle);
    if (host == nullptr || host->display == nullptr) return 0;

    const int display_fd = ConnectionNumber(host->display);
    const auto interval = std::chrono::microseconds(
        1000000u / std::max<std::uint32_t>(1u, std::min<std::uint32_t>(host->input_hz, 240u)));
    auto next_input = std::chrono::steady_clock::now();

    while (! host->stop.load()) {
        while (XPending(host->display) > 0) {
            XEvent event;
            XNextEvent(host->display, &event);
            HandleEvent(*host, event);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_input) {
            PollInput(*host);
            next_input = now + interval;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(display_fd, &readfds);
        int max_fd = display_fd;
        if (host->wake_pipe[0] >= 0) {
            FD_SET(host->wake_pipe[0], &readfds);
            max_fd = std::max(max_fd, host->wake_pipe[0]);
        }

        timeval timeout {};
        timeout.tv_sec  = 0;
        timeout.tv_usec = static_cast<suseconds_t>(std::min<std::int64_t>(interval.count(), 50000));
        const int selected = select(max_fd + 1, &readfds, nullptr, nullptr, &timeout);
        if (selected > 0 && host->wake_pipe[0] >= 0 && FD_ISSET(host->wake_pipe[0], &readfds)) {
            DrainPipe(host->wake_pipe[0]);
        }
    }

    if (host->callbacks.closed != nullptr) host->callbacks.closed(host->callbacks.userdata);
    return 1;
}

void DesktopStop(void* handle) {
    auto* host = static_cast<X11DesktopHost*>(handle);
    if (host == nullptr) return;
    host->stop.store(true);
    WakePipe(host->wake_pipe[1]);
}

void DesktopWake(void* handle) {
    auto* host = static_cast<X11DesktopHost*>(handle);
    if (host == nullptr) return;
    WakePipe(host->wake_pipe[1]);
}

void DesktopActivate(void* handle) {
    auto* host = static_cast<X11DesktopHost*>(handle);
    if (host == nullptr || host->display == nullptr || host->window == 0) return;
    XMapWindow(host->display, host->window);
    XLowerWindow(host->display, host->window);
    XFlush(host->display);
    if (host->callbacks.activated != nullptr) host->callbacks.activated(host->callbacks.userdata);
    WakePipe(host->wake_pipe[1]);
}

void DesktopPresent(void*, void*, std::uint32_t, std::uint32_t) {}

std::uint32_t DesktopPixelWidth(void* handle) {
    auto* host = static_cast<X11DesktopHost*>(handle);
    return host != nullptr ? host->width : 0;
}

std::uint32_t DesktopPixelHeight(void* handle) {
    auto* host = static_cast<X11DesktopHost*>(handle);
    return host != nullptr ? host->height : 0;
}

bool DesktopGetSurfaceInfo(void* handle, DesktopSurfaceInfo& out) {
    auto* host = static_cast<X11DesktopHost*>(handle);
    if (host == nullptr || host->display == nullptr || host->window == 0) return false;

    out.instance_extensions.clear();
    out.instance_extensions.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);
    out.instance_extensions.emplace_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
    out.create_surface = [display = host->display, window = host->window](VkInstance instance,
                                                                          VkSurfaceKHR* surface) {
        VkXlibSurfaceCreateInfoKHR create_info {
            .sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .pNext  = nullptr,
            .flags  = 0,
            .dpy    = display,
            .window = window,
        };
        return vkCreateXlibSurfaceKHR(instance, &create_info, nullptr, surface);
    };
    return true;
}

} // namespace sr::host
