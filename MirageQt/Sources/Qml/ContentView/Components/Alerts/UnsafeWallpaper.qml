import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

// 网页壁纸信任确认对话框：对应 macOS Alerts/UnsafeWallpaper.swift。
// 信任确认流程的状态（pendingTrustAction 等）由窗口持有，
// 组件只负责展示与把确认/取消动作回传给 host。
MirageDialogWindow {
    id: root
    required property var host
    property bool rememberWallpaper: false

    title: "确认运行网页壁纸"
    width: 460
    height: 300
    negativeText: "取消"
    positiveText: "继续运行"
    buttonFlags: FluContentDialogType.NegativeButton | FluContentDialogType.PositiveButton
    onOpened: rememberWallpaper = false
    onNegativeClicked: root.host.cancelWallpaperTrust()
    onPositiveClicked: root.host.confirmWallpaperTrust()
    contentDelegate: Component {
        ColumnLayout {
            width: parent.width
            spacing: 10
            FluText {
                Layout.fillWidth: true
                text: "网页壁纸可能执行来自第三方的脚本。请仅在信任来源和内容时继续运行。"
                wrapMode: Text.WordWrap
            }
            FluToggleSwitch {
                text: "记住对此壁纸的确认"
                checked: root.rememberWallpaper
                clickListener: function () {
                    root.rememberWallpaper = !root.rememberWallpaper;
                }
            }
        }
    }
}
