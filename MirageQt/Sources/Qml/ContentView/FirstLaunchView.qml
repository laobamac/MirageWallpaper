import QtQuick
import QtQuick.Layouts
import FluentUI
import "Components"
import "../GlobalComponents"

MirageDialogWindow {
    id: dialog
    required property var host
    property bool hideUntilUpdate: false
    title: "欢迎使用 Mirage"
    width: 600
    height: 600
    contentMargin: 20
    buttonFlags: 0

    contentDelegate: Component {
        ColumnLayout {
            width: parent.width
            spacing: 12
            FluText {
                Layout.alignment: Qt.AlignHCenter
                text: "欢迎使用 Mirage"
                font: FluTextStyle.Title
            }
            FluDivider { Layout.fillWidth: true }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 30
                Layout.rightMargin: 30
                spacing: 10

                NewSection {
                    icon: FluentIcons.Picture
                    title: "三类壁纸，一站渲染"
                    detail: "支持 Wallpaper Engine 的场景、网页、视频三类壁纸，由专用引擎以独立进程渲染到桌面，兼顾画质与稳定。"
                }
                NewSection {
                    icon: FluentIcons.FolderOpen
                    title: "自动加载创意工坊壁纸"
                    detail: "自动读取 Steam 创意工坊已订阅的壁纸，也可将本地文件夹或视频导入到 Mirage 自有壁纸库。"
                }
                NewSection {
                    icon: FluentIcons.Settings
                    title: "熟悉的界面布局"
                    detail: "沿用 Wallpaper Engine 的界面布局，上手无门槛，并针对 Linux 做了本地化与视觉适配。"
                }
                NewSection {
                    icon: FluentIcons.Edit
                    title: "实时属性调节"
                    detail: "根据壁纸自带的属性动态生成调节控件，音量、速度、颜色、开关等即改即生效。"
                }
            }

            ProjectFeedbackBanner {
                Layout.fillWidth: true
                showsActions: false
            }
            FluToggleSwitch {
                text: "在下次更新前不再显示"
                checked: dialog.hideUntilUpdate
                clickListener: function() { dialog.hideUntilUpdate = !checked; }
            }
            FluFilledButton {
                Layout.alignment: Qt.AlignHCenter
                text: "开始使用"
                onClicked: mirage.completeFirstLaunch(dialog.hideUntilUpdate)
            }
        }
    }

    Component.onCompleted: {
        if (mirage.firstLaunch) open();
    }
    Connections {
        target: mirage
        function onFirstLaunchChanged() {
            if (mirage.firstLaunch) dialog.open();
            else dialog.close();
        }
    }

    component NewSection: RowLayout {
        id: section
        required property int icon
        required property string title
        required property string detail
        Layout.fillWidth: true
        spacing: 12
        FluIcon {
            Layout.preferredWidth: 50
            Layout.preferredHeight: 50
            iconSource: section.icon
            iconSize: 30
            iconColor: FluTheme.primaryColor
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            FluText {
                text: section.title
                font: FluTextStyle.Subtitle
            }
            FluText {
                Layout.fillWidth: true
                text: section.detail
                wrapMode: Text.WordWrap
            }
        }
    }
}
