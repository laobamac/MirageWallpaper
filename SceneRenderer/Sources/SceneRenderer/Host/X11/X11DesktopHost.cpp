#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

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
#include <optional>
#include <span>
#include <thread>
#include <unistd.h>
#include <vector>
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

struct MonitorGeometry {
    int           x { 0 };
    int           y { 0 };
    std::uint32_t width { 0 };
    std::uint32_t height { 0 };
};

enum class PlacementMode
{
    DesktopContainer,
    EwmhDesktopWindow,
    OverrideRedirect
};

struct X11DesktopHost {
    DesktopCallbacks callbacks {};
    Display*         display { nullptr };
    int              screen { 0 };
    Window           root { 0 };
    Window           parent { 0 };
    Window           window { 0 };
    Atom             wm_delete_window { None };
    int              wake_pipe[2] { -1, -1 };
    MonitorGeometry  monitor {};
    std::uint32_t    monitor_index { 0 };
    std::uint32_t    width { 0 };
    std::uint32_t    height { 0 };
    std::uint32_t    input_hz { 60 };
    unsigned int     last_buttons { 0 };
    bool             mouse_inside { false };
    bool             sent_enter { false };
    bool             randr_extension { false };
    bool             use_randr_monitors { false };
    int              randr_event_base { 0 };
    int              randr_error_base { 0 };
    bool             xfixes_extension { false };
    PlacementMode    placement { PlacementMode::OverrideRedirect };
    std::atomic<bool> stop { false };
};

struct LocalGeometry {
    int           x { 0 };
    int           y { 0 };
    std::uint32_t width { 0 };
    std::uint32_t height { 0 };
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

std::vector<Atom> ReadAtomListProperty(Display* display, Window window, Atom property) {
    Atom           actual_type = None;
    int            actual_format = 0;
    unsigned long  nitems = 0;
    unsigned long  bytes_after = 0;
    unsigned char* data = nullptr;
    std::vector<Atom> result;

    const int status = XGetWindowProperty(display,
                                          window,
                                          property,
                                          0,
                                          1024,
                                          False,
                                          XA_ATOM,
                                          &actual_type,
                                          &actual_format,
                                          &nitems,
                                          &bytes_after,
                                          &data);
    if (status == Success && actual_type == XA_ATOM && actual_format == 32 && data != nullptr) {
        const auto* atoms = reinterpret_cast<const Atom*>(data);
        result.assign(atoms, atoms + nitems);
    }
    if (data != nullptr) XFree(data);
    return result;
}

std::optional<Window> ReadWindowProperty(Display* display, Window window, Atom property) {
    Atom           actual_type = None;
    int            actual_format = 0;
    unsigned long  nitems = 0;
    unsigned long  bytes_after = 0;
    unsigned char* data = nullptr;
    std::optional<Window> result;

    const int status = XGetWindowProperty(display,
                                          window,
                                          property,
                                          0,
                                          1,
                                          False,
                                          XA_WINDOW,
                                          &actual_type,
                                          &actual_format,
                                          &nitems,
                                          &bytes_after,
                                          &data);
    if (status == Success && actual_type == XA_WINDOW && actual_format == 32 && nitems == 1 &&
        data != nullptr) {
        result = static_cast<Window>(*reinterpret_cast<unsigned long*>(data));
    }
    if (data != nullptr) XFree(data);
    return result;
}

bool WindowHasAtom(Display* display, Window window, Atom property, Atom atom) {
    auto atoms = ReadAtomListProperty(display, window, property);
    return std::find(atoms.begin(), atoms.end(), atom) != atoms.end();
}

MonitorGeometry RootGeometry(Display* display, Window root) {
    XWindowAttributes attrs {};
    if (XGetWindowAttributes(display, root, &attrs) == 0) return {};
    return {
        .x      = 0,
        .y      = 0,
        .width  = static_cast<std::uint32_t>(std::max(0, attrs.width)),
        .height = static_cast<std::uint32_t>(std::max(0, attrs.height)),
    };
}

bool GetWindowRootOrigin(Display* display, Window root, Window window, int& x, int& y) {
    if (window == root) {
        x = 0;
        y = 0;
        return true;
    }
    Window child = 0;
    return XTranslateCoordinates(display, window, root, 0, 0, &x, &y, &child) != 0;
}

bool Intersects(const MonitorGeometry& a, const MonitorGeometry& b) {
    const int ax2 = a.x + static_cast<int>(a.width);
    const int ay2 = a.y + static_cast<int>(a.height);
    const int bx2 = b.x + static_cast<int>(b.width);
    const int by2 = b.y + static_cast<int>(b.height);
    return a.x < bx2 && ax2 > b.x && a.y < by2 && ay2 > b.y;
}

bool WindowIntersectsMonitor(Display* display, Window root, Window window,
                             const MonitorGeometry& monitor) {
    XWindowAttributes attrs {};
    if (XGetWindowAttributes(display, window, &attrs) == 0) return false;
    if (attrs.map_state != IsViewable || attrs.width <= 0 || attrs.height <= 0) return false;

    int x = 0;
    int y = 0;
    if (! GetWindowRootOrigin(display, root, window, x, y)) return false;
    MonitorGeometry window_geometry {
        .x      = x,
        .y      = y,
        .width  = static_cast<std::uint32_t>(attrs.width),
        .height = static_cast<std::uint32_t>(attrs.height),
    };
    return Intersects(window_geometry, monitor);
}

std::optional<Window> FindDesktopContainerRecursive(Display* display, Window root, Window window,
                                                    Atom window_type_atom,
                                                    Atom desktop_type_atom,
                                                    const MonitorGeometry& monitor,
                                                    int depth) {
    if (depth > 8) return std::nullopt;

    if (window != root && WindowHasAtom(display, window, window_type_atom, desktop_type_atom) &&
        WindowIntersectsMonitor(display, root, window, monitor)) {
        return window;
    }

    Window        query_root = 0;
    Window        query_parent = 0;
    Window*       children = nullptr;
    unsigned int  child_count = 0;
    const Status  ok = XQueryTree(display,
                                 window,
                                 &query_root,
                                 &query_parent,
                                 &children,
                                 &child_count);
    if (ok == 0) return std::nullopt;

    std::optional<Window> result;
    for (unsigned int i = 0; i < child_count && ! result; ++i) {
        result = FindDesktopContainerRecursive(display,
                                               root,
                                               children[i],
                                               window_type_atom,
                                               desktop_type_atom,
                                               monitor,
                                               depth + 1);
    }
    if (children != nullptr) XFree(children);
    return result;
}

std::optional<Window> FindDesktopContainer(Display* display, Window root,
                                           const MonitorGeometry& monitor) {
    const Atom window_type_atom  = InternAtom(display, "_NET_WM_WINDOW_TYPE");
    const Atom desktop_type_atom = InternAtom(display, "_NET_WM_WINDOW_TYPE_DESKTOP");
    return FindDesktopContainerRecursive(display,
                                         root,
                                         root,
                                         window_type_atom,
                                         desktop_type_atom,
                                         monitor,
                                         0);
}

bool HasEwmhWindowManager(Display* display, Window root) {
    const Atom supporting_wm_check = InternAtom(display, "_NET_SUPPORTING_WM_CHECK");
    auto       wm_window = ReadWindowProperty(display, root, supporting_wm_check);
    if (! wm_window || *wm_window == 0) return false;

    auto self_check = ReadWindowProperty(display, *wm_window, supporting_wm_check);
    return self_check && *self_check == *wm_window;
}

void SetSizeHints(X11DesktopHost& host) {
    XSizeHints hints {};
    hints.flags = PPosition | PSize | PMinSize;
    if (host.placement == PlacementMode::DesktopContainer ||
        host.placement == PlacementMode::OverrideRedirect) {
        hints.flags |= PMaxSize;
    }
    hints.x = host.monitor.x;
    hints.y = host.monitor.y;
    hints.width = static_cast<int>(host.monitor.width);
    hints.height = static_cast<int>(host.monitor.height);
    hints.min_width = static_cast<int>(host.monitor.width);
    hints.min_height = static_cast<int>(host.monitor.height);
    hints.max_width = static_cast<int>(host.monitor.width);
    hints.max_height = static_cast<int>(host.monitor.height);
    XSetWMNormalHints(host.display, host.window, &hints);
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

    SetSizeHints(host);
}

void SetClickThrough(X11DesktopHost& host) {
    int event_base = 0;
    int error_base = 0;
    host.xfixes_extension = XFixesQueryExtension(host.display, &event_base, &error_base) != 0;
    if (! host.xfixes_extension) {
        std::cerr << "XFixes is unavailable; wallpaper window will not be click-through.\n";
        return;
    }

    XserverRegion empty_region = XFixesCreateRegion(host.display, nullptr, 0);
    XFixesSetWindowShapeRegion(host.display, host.window, ShapeInput, 0, 0, empty_region);
    XFixesDestroyRegion(host.display, empty_region);
}

std::vector<MonitorGeometry> EnumerateRandrMonitors(Display* display, Window root) {
    int count = 0;
    XRRMonitorInfo* monitors = XRRGetMonitors(display, root, True, &count);
    std::vector<MonitorGeometry> result;
    if (monitors == nullptr) return result;

    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const XRRMonitorInfo& monitor = monitors[i];
        if (monitor.width <= 0 || monitor.height <= 0 || monitor.noutput <= 0) continue;
        result.push_back({
            .x      = monitor.x,
            .y      = monitor.y,
            .width  = static_cast<std::uint32_t>(monitor.width),
            .height = static_cast<std::uint32_t>(monitor.height),
        });
    }
    XRRFreeMonitors(monitors);
    return result;
}

void PrintMonitors(const std::vector<MonitorGeometry>& monitors) {
    std::cerr << "Available XRandR monitors: " << monitors.size() << "\n";
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        std::cerr << "  " << i << ": " << m.width << "x" << m.height << "+"
                  << m.x << "+" << m.y << "\n";
    }
}

bool ConfigureInitialMonitor(X11DesktopHost& host, std::uint32_t requested_index) {
    host.monitor_index = requested_index;
    int version_major = 0;
    int version_minor = 0;
    host.randr_extension =
        XRRQueryExtension(host.display, &host.randr_event_base, &host.randr_error_base) != 0 &&
        XRRQueryVersion(host.display, &version_major, &version_minor) != 0 &&
        (version_major > 1 || (version_major == 1 && version_minor >= 5));

    if (host.randr_extension) {
        auto monitors = EnumerateRandrMonitors(host.display, host.root);
        if (! monitors.empty()) {
            host.use_randr_monitors = true;
            if (requested_index >= monitors.size()) {
                std::cerr << "--screen " << requested_index << " is out of range. ";
                PrintMonitors(monitors);
                return false;
            }
            host.monitor = monitors[requested_index];
            return true;
        }
    }

    host.use_randr_monitors = false;
    if (requested_index != 0) {
        std::cerr << "XRandR monitors are unavailable; only --screen 0 can be used.\n";
        return false;
    }

    host.monitor = RootGeometry(host.display, host.root);
    if (host.monitor.width == 0 || host.monitor.height == 0) {
        std::cerr << "Failed to query X11 root window geometry.\n";
        return false;
    }
    return true;
}

bool RefreshMonitorGeometry(X11DesktopHost& host) {
    MonitorGeometry next {};
    if (host.use_randr_monitors) {
        auto monitors = EnumerateRandrMonitors(host.display, host.root);
        if (host.monitor_index >= monitors.size()) {
            std::cerr << "Selected XRandR monitor " << host.monitor_index
                      << " is no longer available. ";
            PrintMonitors(monitors);
            return false;
        }
        next = monitors[host.monitor_index];
    } else {
        next = RootGeometry(host.display, host.root);
        if (next.width == 0 || next.height == 0) return false;
    }

    const bool changed = next.x != host.monitor.x || next.y != host.monitor.y ||
                         next.width != host.monitor.width || next.height != host.monitor.height;
    host.monitor = next;
    host.width   = next.width;
    host.height  = next.height;
    return changed;
}

LocalGeometry WindowLocalGeometry(const X11DesktopHost& host) {
    int parent_x = 0;
    int parent_y = 0;
    GetWindowRootOrigin(host.display, host.root, host.parent, parent_x, parent_y);
    return {
        .x      = host.monitor.x - parent_x,
        .y      = host.monitor.y - parent_y,
        .width  = host.monitor.width,
        .height = host.monitor.height,
    };
}

void ApplyWindowGeometry(X11DesktopHost& host) {
    if (host.window == 0) return;
    auto geometry = WindowLocalGeometry(host);
    XMoveResizeWindow(host.display,
                      host.window,
                      geometry.x,
                      geometry.y,
                      geometry.width,
                      geometry.height);
    SetSizeHints(host);
    XLowerWindow(host.display, host.window);
    XFlush(host.display);
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
    int root_x = 0;
    int root_y = 0;
    int win_x = 0;
    int win_y = 0;
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

    const int local_x = root_x - host.monitor.x;
    const int local_y = root_y - host.monitor.y;
    const bool inside = local_x >= 0 && local_y >= 0 &&
                        local_x < static_cast<int>(host.monitor.width) &&
                        local_y < static_cast<int>(host.monitor.height);
    EmitMouseEnter(host, inside);
    if (inside && host.callbacks.mouse_move != nullptr && host.monitor.width > 0 &&
        host.monitor.height > 0) {
        const double x = std::clamp(static_cast<double>(local_x) / host.monitor.width, 0.0, 1.0);
        const double y = std::clamp(static_cast<double>(local_y) / host.monitor.height, 0.0, 1.0);
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

void RefreshAndApplyGeometry(X11DesktopHost& host) {
    if (! RefreshMonitorGeometry(host)) {
        host.stop.store(true);
        WakePipe(host.wake_pipe[1]);
        return;
    }
    ApplyWindowGeometry(host);
}

bool IsRandrEvent(const X11DesktopHost& host, int event_type) {
    return host.randr_extension && event_type >= host.randr_event_base &&
           event_type < host.randr_event_base + RRNumberEvents;
}

void HandleEvent(X11DesktopHost& host, XEvent& event) {
    if (IsRandrEvent(host, event.type)) {
        if (event.type == host.randr_event_base + RRScreenChangeNotify) {
            XRRUpdateConfiguration(&event);
        }
        RefreshAndApplyGeometry(host);
        return;
    }

    switch (event.type) {
    case ConfigureNotify:
        if (event.xconfigure.window == host.window) {
            if (event.xconfigure.width > 0) {
                host.width = static_cast<std::uint32_t>(event.xconfigure.width);
            }
            if (event.xconfigure.height > 0) {
                host.height = static_cast<std::uint32_t>(event.xconfigure.height);
            }
        } else if (event.xconfigure.window == host.root || event.xconfigure.window == host.parent) {
            RefreshAndApplyGeometry(host);
        }
        break;
    case ClientMessage:
        if (static_cast<Atom>(event.xclient.data.l[0]) == host.wm_delete_window) {
            host.stop.store(true);
        }
        break;
    case DestroyNotify:
        if (event.xdestroywindow.window == host.window || event.xdestroywindow.window == host.parent) {
            host.stop.store(true);
        }
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
    const bool wayland_session = IsWaylandSession();
    if (wayland_session && ! EnvEnabled("SCENERENDERER_FORCE_X11")) {
        std::cerr << "SceneWallpaper Linux MVP requires an X11 session. "
                     "Wayland wallpaper support is compositor-specific and is not supported yet.\n";
        return nullptr;
    }
    if (wayland_session) {
        std::cerr << "SCENERENDERER_FORCE_X11 is set; using XWayland for debugging only. "
                     "Desktop-layer behavior is not guaranteed under Wayland.\n";
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
    host->parent    = host->root;
    host->input_hz  = config != nullptr && config->input_hz > 0 ? config->input_hz : 60u;

    const std::uint32_t requested_screen = config != nullptr ? config->screen_index : 0u;
    if (! ConfigureInitialMonitor(*host, requested_screen)) {
        CloseHost(host);
        return nullptr;
    }
    host->width  = host->monitor.width;
    host->height = host->monitor.height;

    if (host->randr_extension) {
        XRRSelectInput(host->display,
                       host->root,
                       RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                           RROutputChangeNotifyMask | RRResourceChangeNotifyMask);
    }
    XSelectInput(host->display, host->root, PropertyChangeMask | StructureNotifyMask);

    if (auto desktop_container = FindDesktopContainer(display, host->root, host->monitor)) {
        host->parent = *desktop_container;
        host->placement = PlacementMode::DesktopContainer;
        XSelectInput(host->display, host->parent, StructureNotifyMask | PropertyChangeMask);
    } else if (HasEwmhWindowManager(display, host->root)) {
        host->placement = PlacementMode::EwmhDesktopWindow;
    } else {
        host->placement = PlacementMode::OverrideRedirect;
    }

    XSetWindowAttributes attrs {};
    attrs.background_pixel = BlackPixel(display, host->screen);
    attrs.border_pixel     = BlackPixel(display, host->screen);
    attrs.event_mask       = StructureNotifyMask | PropertyChangeMask | ExposureMask;

    unsigned long value_mask = CWBackPixel | CWBorderPixel | CWEventMask;
    if (host->placement == PlacementMode::OverrideRedirect) {
        attrs.override_redirect = True;
        value_mask |= CWOverrideRedirect;
    }

    auto geometry = WindowLocalGeometry(*host);
    host->window = XCreateWindow(display,
                                 host->parent,
                                 geometry.x,
                                 geometry.y,
                                 geometry.width,
                                 geometry.height,
                                 0,
                                 CopyFromParent,
                                 InputOutput,
                                 CopyFromParent,
                                 value_mask,
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
    SetClickThrough(*host);

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
