import QtQuick
import QtQuick.Layouts
import FluentUI
import "../ContentViewLogic.js" as ContentViewLogic

// 三个方形网格共用的紧凑分页器：对齐 macOS 的深色横条、
// 蓝色页码与双箭头样式。分页器只承担直接页码选择，不混入
// 页码输入或图标尺寸偏好，保证在网格底部保持与 macOS 一致的小尺寸。
FluFrame {
    id: controls

    // 输入接口均为从 1 开始的整数页码；调用方保持
    // currentPage <= pageCount，selected 仅在 GUI 线程的 QML 事件中发出。
    property int currentPage: 1
    property int pageCount: 1
    signal selected(int page)

    implicitWidth: navigator.implicitWidth + 8
    implicitHeight: 28
    radius: 3
    color: FluTheme.dark
        ? Qt.rgba(43 / 255, 43 / 255, 47 / 255, 0.96)
        : Qt.rgba(238 / 255, 238 / 255, 240 / 255, 0.96)
    border.width: 0

    RowLayout {
        id: navigator
        anchors.centerIn: parent
        spacing: 2

        FluButton {
            Layout.preferredWidth: 32
            Layout.preferredHeight: 22
            horizontalPadding: 0
            text: "«"
            contentDescription: qsTr("上一页")
            font: FluTextStyle.BodyStrong
            enabled: controls.currentPage > 1
            normalColor: "transparent"
            hoverColor: FluTheme.dark
                ? Qt.rgba(68 / 255, 68 / 255, 72 / 255, 1)
                : Qt.rgba(218 / 255, 218 / 255, 222 / 255, 1)
            disableColor: "transparent"
            dividerColor: normalColor
            // macOS 两侧箭头保持统一蓝色；边界页仅禁止点击，
            // 不改变整个分页条的颜色节奏。
            textColor: FluTheme.primaryColor
            onClicked: controls.selected(controls.currentPage - 1)
        }

        Repeater {
            model: ContentViewLogic.wallpaperPageItems(controls.currentPage, controls.pageCount)
            delegate: Item {
                required property int modelData
                Layout.preferredWidth: modelData === 0
                    ? 14 : Math.max(20, String(modelData).length * 8 + 6)
                Layout.preferredHeight: 22

                FluText {
                    anchors.centerIn: parent
                    visible: parent.modelData === 0
                    text: "…"
                    color: FluTheme.primaryColor
                    font: FluTextStyle.Caption
                }

                FluButton {
                    anchors.fill: parent
                    horizontalPadding: 0
                    visible: parent.modelData !== 0
                    text: String(parent.modelData)
                    contentDescription: qsTr("第 %1 页").arg(parent.modelData)
                    font: FluTextStyle.Caption
                    normalColor: parent.modelData === controls.currentPage
                        ? (FluTheme.dark
                            ? Qt.rgba(82 / 255, 82 / 255, 88 / 255, 1)
                            : Qt.rgba(205 / 255, 205 / 255, 210 / 255, 1))
                        : "transparent"
                    dividerColor: normalColor
                    hoverColor: parent.modelData === controls.currentPage
                        ? normalColor
                        : (FluTheme.dark
                            ? Qt.rgba(68 / 255, 68 / 255, 72 / 255, 1)
                            : Qt.rgba(218 / 255, 218 / 255, 222 / 255, 1))
                    textColor: parent.modelData === controls.currentPage
                        ? FluTheme.fontPrimaryColor : FluTheme.primaryColor
                    onClicked: controls.selected(parent.modelData)
                }
            }
        }

        FluButton {
            Layout.preferredWidth: 32
            Layout.preferredHeight: 22
            horizontalPadding: 0
            text: "»"
            contentDescription: qsTr("下一页")
            font: FluTextStyle.BodyStrong
            enabled: controls.currentPage < controls.pageCount
            normalColor: "transparent"
            hoverColor: FluTheme.dark
                ? Qt.rgba(68 / 255, 68 / 255, 72 / 255, 1)
                : Qt.rgba(218 / 255, 218 / 255, 222 / 255, 1)
            disableColor: "transparent"
            dividerColor: normalColor
            textColor: FluTheme.primaryColor
            onClicked: controls.selected(controls.currentPage + 1)
        }
    }
}
