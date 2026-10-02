import QtQuick
import org.kde.taskmanager 0.1 as TaskManager

/*
 * Computes desktop-window facts for the wallpaper display item, mirroring the
 * KWin scripting API concepts (active window, fullscreen, maximized, frame
 * geometry) through org.kde.taskmanager, which is backed by the active
 * KWin/Mutter workspace and behaves identically on Plasma X11 and Wayland.
 *
 * The ACTIVE window is found by scanning the model rows for the IsActive role
 * (the same scan TasksModel::activeTask() performs), then only windows whose
 * frame geometry intersects this wallpaper's screen area count. This keeps
 * each screen's wallpaper independent: a window focused on another screen does
 * not pause this screen's wallpaper.
 *
 * TasksModel's own screen filter is deliberately NOT used: it compares the
 * window's screen geometry with screenGeometry for exact equality, which
 * silently drops every window when the two rects differ by a rounding or
 * scaling step (mixed DPI, fractional scale), leaving the model empty. The
 * per-screen scoping is done here with a non-empty intersection instead.
 *
 * Flag bits (sent verbatim as WINDOW_STATE { u32 flags }):
 *   0x1 covered    - the active window's frame geometry intersects the
 *                    wallpaper area (desktop at least partially covered)
 *   0x2 focusLost  - a normal window is active on this screen (desktop lost
 *                    focus)
 *   0x4 maximized  - the active window is maximized
 *   0x8 fullscreen - a non-minimized window on this screen is fullscreen or
 *                    maximized, or its frame geometry fills the screen
 *                    (wallpaper fully covered; independent of focus. On Linux
 *                    a maximized window counts as fullscreen because it covers
 *                    the wallpaper the same way)
 */
Item {
    id: root

    // Wallpaper area in global screen coordinates; supplied by main.qml from
    // the Qt Screen object so the geometry comparison shares one coordinate
    // space with the TaskManager Geometry role.
    property var screenGeometry
    readonly property int flags: stateFlags
    readonly property int taskCount: tasks.count
    // Diagnostics for the ShowDiagnostics overlay: the per-row decision inputs
    // (W=IsWindow, A=IsActive, act=on current activity, hit=geometry reaches
    // this screen, g=geometry) plus how often recompute() ran and the screen
    // rect it compares against.
    property string debugInfo: ""
    property int debugRecomputeCount: 0
    readonly property string debugScreen: root.screenGeometry
        ? root.screenGeometry.x.toFixed(0) + "," + root.screenGeometry.y.toFixed(0)
          + " " + root.screenGeometry.width.toFixed(0) + "x" + root.screenGeometry.height.toFixed(0)
        : "null"
    property int stateFlags: 0

    TaskManager.ActivityInfo { id: activityInfo }
    TaskManager.VirtualDesktopInfo { id: desktopInfo }

    TaskManager.TasksModel {
        id: tasks
        sortMode: TaskManager.TasksModel.SortVirtualDesktop
        groupMode: TaskManager.TasksModel.GroupDisabled
        filterByVirtualDesktop: true
        virtualDesktop: desktopInfo.currentDesktop

        onActiveTaskChanged: root.recompute()
        onDataChanged: root.recompute()
        onCountChanged: root.recompute()
    }

    // Activity and desktop switches change which window is active without
    // touching the task rows, so recompute on those signals as well.
    Connections {
        target: activityInfo
        function onCurrentActivityChanged() { root.recompute(); }
    }
    Connections {
        target: desktopInfo
        function onCurrentDesktopChanged() { root.recompute(); }
    }

    function role(index, roleId) {
        return tasks.data(index, roleId);
    }

    // Formats a geometry component defensively: a non-number value (e.g. an
    // unconverted role payload) prints "?" instead of raising a QML error.
    function fmtNum(value) {
        return typeof value === "number" ? value.toFixed(0) : "?";
    }

    // Manual rect intersection. Property access cannot throw, and a value that
    // is not a rect degrades to "no intersection" instead of aborting
    // recompute(), so the diagnostics dump always completes.
    function intersectsRect(a, b) {
        if (!a || !b) return false;
        return a.x < b.x + b.width && a.x + a.width > b.x
            && a.y < b.y + b.height && a.y + a.height > b.y;
    }

    // 几何"占满屏幕"判定，对齐 macOS appIsFullscreen：窗口边缘与屏幕边缘
    // 在容差内贴合，或相交面积 ≥98.5% 屏幕面积且 ≥90% 窗口面积。最大化
    // （无面板时即占满全屏）等窗口因此同样计入全屏位。
    function isFullscreenGeometry(geometry, screen) {
        if (!geometry || !screen || screen.width <= 0 || screen.height <= 0)
            return false;
        const tolerance = Math.max(4, Math.min(screen.width, screen.height) * 0.005);
        const edgesMatch = Math.abs(geometry.x - screen.x) <= tolerance
            && Math.abs(geometry.y - screen.y) <= tolerance
            && Math.abs(geometry.x + geometry.width - (screen.x + screen.width)) <= tolerance
            && Math.abs(geometry.y + geometry.height - (screen.y + screen.height)) <= tolerance;
        if (edgesMatch) return true;
        const ix = Math.max(0, Math.min(geometry.x + geometry.width,
                                        screen.x + screen.width)
                                  - Math.max(geometry.x, screen.x));
        const iy = Math.max(0, Math.min(geometry.y + geometry.height,
                                        screen.y + screen.height)
                                  - Math.max(geometry.y, screen.y));
        const screenArea = screen.width * screen.height;
        const windowArea = geometry.width * geometry.height;
        return windowArea > 0
            && (ix * iy) / screenArea >= 0.985
            && (ix * iy) / windowArea >= 0.90;
    }

    function recompute() {
        ++debugRecomputeCount;
        let next = 0;
        const activity = activityInfo.currentActivity;
        const parts = [];
        let fullscreenSeen = false;
        for (let i = 0; i < tasks.count; ++i) {
            const index = tasks.makeModelIndex(i);
            const isWindow = role(index, TaskManager.AbstractTasksModel.IsWindow);
            const isActive = role(index, TaskManager.AbstractTasksModel.IsActive);
            const isFullScreen = role(index, TaskManager.AbstractTasksModel.IsFullScreen);
            const isMaximized = role(index, TaskManager.AbstractTasksModel.IsMaximized);
            const geometry = role(index, TaskManager.AbstractTasksModel.Geometry);
            const acts = role(index, TaskManager.AbstractTasksModel.Activities);
            const onActivity = !acts || !acts.length || acts.indexOf(activity) !== -1;
            const hits = intersectsRect(geometry, root.screenGeometry);
            parts.push(i + ":W" + (isWindow === true ? 1 : 0)
                + "A" + (isActive === true ? 1 : 0)
                + "act" + (onActivity ? 1 : 0)
                + "hit" + (hits ? 1 : 0)
                + "fs" + (isFullScreen === true ? 1 : 0)
                + "mx" + (isMaximized === true ? 1 : 0)
                + "g" + (geometry
                    ? fmtNum(geometry.x) + "," + fmtNum(geometry.y)
                      + " " + fmtNum(geometry.width) + "x" + fmtNum(geometry.height)
                    : "null"));
            if (isWindow !== true || !onActivity || !hits)
                continue;
            if (isActive === true) {
                next |= 1;
                next |= 2;
                if (isMaximized === true)
                    next |= 4;
            }
            // 全屏覆盖不依赖焦点：任何本屏、非最小化的窗口，只要处于全屏
            // 或最大化（Linux 下最大化视同全屏：最大化窗口同样占满并盖住
            // 壁纸，与 macOS 的几何判定语义不同），或几何上占满屏幕，都
            // 应触发全屏位。
            const fullscreenCovering = isFullScreen === true || isMaximized === true
                || isFullscreenGeometry(geometry, root.screenGeometry);
            if (fullscreenCovering
                && role(index, TaskManager.AbstractTasksModel.IsMinimized) !== true) {
                fullscreenSeen = true;
            }
        }
        if (fullscreenSeen)
            next |= 8;
        debugInfo = parts.join("  ");
        stateFlags = next;
    }

    Component.onCompleted: recompute()
    onScreenGeometryChanged: recompute()
}
