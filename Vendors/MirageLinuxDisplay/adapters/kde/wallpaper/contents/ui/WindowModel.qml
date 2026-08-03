import QtQuick
import org.kde.taskmanager 0.1 as TaskManager

Item {
    id: root

    property var screenGeometry
    readonly property int flags: stateFlags
    property int stateFlags: 0

    TaskManager.ActivityInfo { id: activityInfo }
    TaskManager.VirtualDesktopInfo { id: desktopInfo }

    TaskManager.TasksModel {
        id: tasks
        sortMode: TaskManager.TasksModel.SortVirtualDesktop
        groupMode: TaskManager.TasksModel.GroupDisabled
        filterByVirtualDesktop: true
        virtualDesktop: desktopInfo.currentDesktop
        filterByScreen: true
        screenGeometry: root.screenGeometry

        onActiveTaskChanged: root.recompute()
        onDataChanged: root.recompute()
        onCountChanged: root.recompute()
    }

    // Plasma's task model is backed by the active KWin/Mutter workspace. It
    // is intentionally used instead of X11 window enumeration so this item
    // behaves identically on Plasma X11 and Plasma Wayland.
    Connections {
        target: activityInfo
        function onCurrentActivityChanged() { root.recompute(); }
    }
    Connections {
        target: desktopInfo
        function onCurrentDesktopChanged() { root.recompute(); }
    }

    function role(index, name) {
        return tasks.data(index, TaskManager.AbstractTasksModel[name]);
    }

    function recompute() {
        let next = 0;
        const activity = activityInfo.currentActivity;
        for (let i = 0; i < tasks.count; ++i) {
            const index = tasks.makeModelIndex(i);
            if (role(index, "IsWindow") !== true)
                continue;
            const activities = role(index, "Activities");
            if (activities && activities.length && activities.indexOf(activity) === -1)
                continue;
            if (role(index, "IsMinimized") === true)
                continue;
            next |= 1;
            if (role(index, "IsActive") === true)
                next |= 2;
            if (role(index, "IsFullScreen") === true)
                next |= 8;
            else if (role(index, "IsMaximized") === true)
                next |= 4;
        }
        stateFlags = next;
    }

    Component.onCompleted: recompute()
    onScreenGeometryChanged: recompute()
}
