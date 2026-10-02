// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Layouts

ColumnLayout {
    spacing: 22
    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        spacing: 22
        Image {
            source: "qrc:/art/AppIcon.png"
            Layout.preferredWidth: 88
            Layout.preferredHeight: 88
            fillMode: Image.PreserveAspectFit
        }
        Rectangle {
            width: 1
            height: 90
            color: Theme.line
        }
        ColumnLayout {
            MText {
                text: "Mirage"
                font.pixelSize: 32
                font.bold: true
            }
            MText {
                text: Theme.t("Linux 动态壁纸引擎")
                color: Theme.secondary
            }
            MText {
                text: Theme.t("场景 · 网页 · 视频")
                font.pixelSize: 11
                color: Theme.secondary
            }
        }
    }
    MText {
        text: "1.0.0 · " + Theme.t("开发构建")
        color: Theme.secondary
        Layout.alignment: Qt.AlignHCenter
    }
    RowLayout {
        Layout.alignment: Qt.AlignHCenter
        MButton {
            text: "GitHub"
            symbol: "link"
            onClicked: Qt.openUrlExternally("https://github.com/laobamac/MirageWallpaper")
        }
        MButton {
            text: Theme.t("提交 Issue")
            symbol: "edit"
            onClicked: Qt.openUrlExternally("https://github.com/laobamac/MirageWallpaper/issues/new/choose")
        }
    }
    FormSection {
        title: "开发团队"
        symbol: "person"
        Repeater {
            model: [
                {
                    name: "Xiaoci Wang",
                    user: "laobamac",
                    role: "项目作者 · 开发者"
                },
                {
                    name: "Jiale Yu",
                    user: "dawalishi821",
                    role: "开发者"
                },
                {
                    name: "Pikachu Ren",
                    user: "PIKACHUIM",
                    role: "开发者"
                },
                {
                    name: "Yinan Qin",
                    user: "elysia-best",
                    role: "开发者"
                }
            ]
            RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 12
                MText {
                    text: modelData.name
                    font.weight: Font.DemiBold
                    Layout.preferredWidth: 130
                }
                MText {
                    text: Theme.t(modelData.role)
                    color: Theme.secondary
                    Layout.fillWidth: true
                }
                MButton {
                    text: "@" + modelData.user
                    flatStyle: true
                    onClicked: Qt.openUrlExternally("https://github.com/" + modelData.user)
                }
            }
        }
    }
    FormSection {
        title: "支持 Mirage"
        symbol: "heart"
        MText {
            text: Theme.t("Mirage 会继续免费开放开发。若它为你的桌面带来了价值，欢迎按自己的意愿赞助；每一份支持都会用于持续维护与兼容性改进。")
            Layout.fillWidth: true
            color: Theme.secondary
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Repeater {
                model: [
                    {
                        file: "afdian.jpg",
                        label: "爱发电"
                    },
                    {
                        file: "wechat-pay.png",
                        label: "微信支付"
                    },
                    {
                        file: "alipay.jpg",
                        label: "支付宝"
                    }
                ]
                ColumnLayout {
                    required property var modelData
                    Layout.preferredWidth: 118
                    Image {
                        source: "qrc:/art/" + modelData.file
                        Layout.preferredWidth: 118
                        Layout.preferredHeight: 140
                        fillMode: Image.PreserveAspectFit
                        TapHandler {
                            onTapped: {
                                qr.source = parent.source;
                                qrSheet.open();
                            }
                        }
                    }
                    MText {
                        text: Theme.t(modelData.label)
                        font.bold: true
                        Layout.alignment: Qt.AlignHCenter
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                MText {
                    text: "USDT"
                    font.bold: true
                }
                MText {
                    text: Theme.t("海外赞助")
                    color: Theme.secondary
                }
                MText {
                    text: "0xFc0a5C52e3A085FEc7b077FE3D2C413114Bf880D"
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                    Layout.fillWidth: true
                }
                MButton {
                    text: Theme.t("复制地址")
                    onClicked: bridge.copyText("0xFc0a5C52e3A085FEc7b077FE3D2C413114Bf880D")
                }
            }
        }
    }
    Sheet {
        id: qrSheet
        width: 360
        contentItem: ColumnLayout {
            Image {
                id: qr
                Layout.preferredWidth: 320
                Layout.preferredHeight: 380
                fillMode: Image.PreserveAspectFit
            }
            MButton {
                text: Theme.t("关闭")
                Layout.alignment: Qt.AlignHCenter
                onClicked: qrSheet.close()
            }
        }
    }
}
