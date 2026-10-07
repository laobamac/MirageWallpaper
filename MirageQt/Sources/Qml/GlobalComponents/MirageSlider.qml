import QtQuick
import QtQuick.Templates as T
import FluentUI

// 自定义刻度滑块（对齐 macOS GlobalComponents/MirageSlider.swift）：
// 圆角轨道 + 主题色进度填充 + 圆形白色 thumb（悬停放大 1.06、拖拽放大 1.12、
// 带阴影）；stepSize > 0 时拖动吸附到步进值。
// 基于 T.Slider 实现，保留 Slider 的 from/to/value/stepSize 与
// onMoved/onValueChanged 等 API，可无缝替换现有 FluSlider 使用处。
T.Slider {
    id: control

    implicitWidth: 180
    implicitHeight: 28
    padding: 0

    // macOS 尺寸：轨道 5px、thumb 16px。
    readonly property real __trackHeight: 5
    readonly property real __thumbSize: 16

    // 进度填充在悬停/拖拽时更亮（对齐 accentColor.opacity 0.85→1.0）。
    readonly property real __fillOpacity: (control.pressed || control.hovered) ? 1.0 : 0.85

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: control.__thumbSize
        implicitHeight: control.__thumbSize
        radius: control.__thumbSize / 2
        color: "white"
        border.color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.2) : Qt.rgba(0, 0, 0, 0.08)
        scale: control.pressed ? 1.12 : (control.hovered ? 1.06 : 1.0)
        Behavior on scale {
            NumberAnimation {
                duration: 120
                easing.type: Easing.OutCubic
            }
        }
        FluShadow {
            radius: control.__thumbSize / 2
        }
    }

    background: Item {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - control.__trackHeight) / 2
        implicitWidth: 180
        implicitHeight: control.__trackHeight
        width: control.availableWidth
        height: control.__trackHeight

        // 轨道底色。
        Rectangle {
            anchors.fill: parent
            radius: control.__trackHeight / 2
            color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.14)
        }
        // 进度填充：从起点到 thumb 中心（对齐 macOS 的 x + thumbSize/2）。
        Rectangle {
            width: control.position * parent.width
            height: parent.height
            radius: control.__trackHeight / 2
            color: FluTheme.primaryColor
            opacity: control.__fillOpacity
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }
        }
    }
}
