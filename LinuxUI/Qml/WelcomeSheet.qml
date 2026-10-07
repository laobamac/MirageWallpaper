// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Layouts

Sheet {
    id: sheet
    width: 600
    contentItem: ColumnLayout {
        spacing: 18
        MText {
            text: Theme.t("欢迎使用 Mirage")
            font.pixelSize: 28
            Layout.alignment: Qt.AlignHCenter
        }
        Rectangle {
            height: 1
            Layout.fillWidth: true
            color: Theme.line
        }
        Repeater {
            model: [
                {
                    icon: "grid",
                    color: "#af52de",
                    title: "三类壁纸，一站渲染",
                    body: "支持 Wallpaper Engine 的场景、网页、视频三类壁纸，由专用引擎以独立进程渲染到桌面，兼顾画质与稳定。"
                },
                {
                    icon: "download",
                    color: "#007aff",
                    title: "自动加载创意工坊壁纸",
                    body: "自动读取 Steam 创意工坊已订阅的壁纸，也可将本地文件夹或视频导入到 Mirage 自有壁纸库。"
                },
                {
                    icon: "display",
                    color: "#ffb300",
                    title: "熟悉的界面布局",
                    body: "保留 Mirage 的界面布局、操作方式与个性化设置。"
                },
                {
                    icon: "sliders",
                    color: "#34c759",
                    title: "实时属性调节",
                    body: "根据壁纸自带的属性动态生成调节控件，音量、速度、颜色、开关等即改即生效。"
                }
            ]
            RowLayout {
                required property var modelData
                Layout.fillWidth: true
                Layout.leftMargin: 30
                Layout.rightMargin: 30
                spacing: 18
                Icon {
                    name: modelData.icon
                    color: modelData.color
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    MText {
                        text: Theme.t(modelData.title)
                        font.pixelSize: 19
                        font.bold: true
                    }
                    MText {
                        text: Theme.t(modelData.body)
                        Layout.fillWidth: true
                    }
                }
            }
        }
        MText {
            text: Theme.t("Mirage 仍处于早期阶段")
            color: Theme.orange
            Layout.alignment: Qt.AlignHCenter
        }
        MButton {
            text: Theme.t("开始使用")
            implicitWidth: 120
            prominent: true
            Layout.alignment: Qt.AlignHCenter
            onClicked: {
                bridge.save("welcomeDismissed", hideNext.checked);
                sheet.close();
            }
        }
        MCheck {
            id: hideNext
            text: Theme.t("在下次更新前不再显示")
            checked: true
        }
    }
}
