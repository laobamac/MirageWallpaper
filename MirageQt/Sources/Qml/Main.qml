import QtQuick
import FluentUI
import "ContentView"

// main.qml 承担 macOS AppDelegate.swift 的角色：应用生命周期编排层。
// - 窗口内容由 ContentView 提供（对应 macOS ContentView.swift），
//   MainWindow.qml 那层薄包装已移除；
// - 应用级启动动作（对应 applicationDidFinishLaunching 的 QML 侧部分）
//   在 Component.onCompleted 中执行；渲染恢复 / Steam session 刷新等
//   C++ 侧逻辑由 MirageController 构造时负责（见 C++ 重构阶段）；
// - 退出清理（对应 applicationWillTerminate）经 aboutToQuit 接线。
ContentView {
    id: window

    function applyAppearance(appearance) {
        if (appearance === "light") {
            FluTheme.darkMode = FluThemeType.Light;
        } else if (appearance === "dark") {
            FluTheme.darkMode = FluThemeType.Dark;
        } else {
            FluTheme.darkMode = FluThemeType.System;
        }
    }

    Component.onCompleted: {
        FluTheme.animationEnabled = true;
        applyAppearance(mirage.settings.appearance);
        // 启动编排：首次启动引导等窗口级初始化。
        // 渲染恢复 / 播放列表轮转 / Steam session 刷新由 MirageController 负责。
    }

    Connections {
        target: mirage
        function onSettingsChanged() {
            applyAppearance(mirage.settings.appearance);
        }
    }

    Connections {
        target: Qt.application
        function onAboutToQuit() {
            // 退出清理：渲染器停止、运行时状态保存由 MirageController 析构完成，
            // 此处预留 QML 侧清理（如未提交草稿丢弃）的挂接点。
        }
    }
}
