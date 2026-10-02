// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Sheet {
    id: sheet
    width: 620
    property int step: 0
    property bool passwordVisible: false
    property int loginMode: 0
    contentItem: ColumnLayout {
        spacing: 18
        RowLayout {
            Item {
                Layout.fillWidth: true
            }
            MButton {
                text: Theme.t("关闭")
                onClicked: sheet.close()
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 18
            Repeater {
                model: ["欢迎", "登录", "完成"]
                RowLayout {
                    required property string modelData
                    required property int index
                    Rectangle {
                        width: 28
                        height: 28
                        radius: 14
                        color: sheet.step >= index ? Theme.accent : Theme.line
                        Text {
                            anchors.centerIn: parent
                            text: String(index + 1)
                            color: "white"
                        }
                    }
                    MText {
                        text: Theme.t(modelData)
                    }
                }
            }
        }
        Rectangle {
            height: 1
            Layout.fillWidth: true
            color: Theme.line
        }
        StackLayout {
            currentIndex: sheet.step
            Layout.preferredHeight: 360
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 18
                Icon {
                    name: "cloud"
                    Layout.preferredWidth: 64
                    Layout.preferredHeight: 64
                    Layout.alignment: Qt.AlignHCenter
                    color: Theme.accent
                }
                MText {
                    text: Theme.t("连接 Steam 创意工坊")
                    font.pixelSize: 24
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }
                MText {
                    text: Theme.t("登录 Steam 后，可以浏览订阅、管理收藏并下载壁纸。")
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.secondary
                }
                MText {
                    text: Theme.t("Steam 服务尚未连接")
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.orange
                }
                Item {
                    Layout.fillHeight: true
                }
            }
            ColumnLayout {
                spacing: 14
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    MButton {
                        text: Theme.t("账户密码")
                        prominent: sheet.loginMode === 0
                        onClicked: sheet.loginMode = 0
                    }
                    MButton {
                        text: Theme.t("扫码登录")
                        prominent: sheet.loginMode === 1
                        onClicked: sheet.loginMode = 1
                    }
                }
                ColumnLayout {
                    visible: sheet.loginMode === 0
                    Layout.fillWidth: true
                    Layout.leftMargin: 50
                    Layout.rightMargin: 50
                    MText {
                        text: Theme.t("Steam 账户")
                    }
                    MField {
                        id: username
                        Layout.fillWidth: true
                        placeholderText: Theme.t("全球 Steam 登录账户名（非昵称）")
                    }
                    MText {
                        text: Theme.t("密码")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        MField {
                            id: password
                            Layout.fillWidth: true
                            placeholderText: Theme.t("密码")
                            echoMode: sheet.passwordVisible ? TextInput.Normal : TextInput.Password
                        }
                        MButton {
                            symbol: "lock"
                            onClicked: sheet.passwordVisible = !sheet.passwordVisible
                        }
                    }
                    MCheck {
                        text: Theme.t("记住登录状态")
                        checked: true
                    }
                    MButton {
                        text: Theme.t("登录")
                        prominent: true
                        Layout.fillWidth: true
                        enabled: Store.steamConnected && username.text !== "" && password.text !== ""
                        onClicked: Store.request("login", {
                            username: username.text,
                            password: password.text
                        })
                    }
                }
                EmptyState {
                    visible: sheet.loginMode === 1
                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    symbol: "grid"
                    title: Theme.t("等待登录二维码")
                    subtitle: Theme.t("Steam 服务尚未连接")
                }
                MField {
                    visible: Store.steamConnected
                    Layout.fillWidth: true
                    placeholderText: Theme.t("Steam Guard 验证码")
                }
                MText {
                    text: Theme.t("Steam 服务尚未连接")
                    visible: !Store.steamConnected
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.secondary
                }
                Item {
                    Layout.fillHeight: true
                }
            }
            EmptyState {
                title: Theme.t("登录完成")
                symbol: "check"
                subtitle: Theme.t("现在可以浏览和下载创意工坊壁纸")
            }
        }
        Rectangle {
            height: 1
            Layout.fillWidth: true
            color: Theme.line
        }
        RowLayout {
            MButton {
                text: Theme.t("上一步")
                symbol: "left"
                visible: sheet.step > 0
                onClicked: sheet.step--
            }
            Item {
                Layout.fillWidth: true
            }
            MButton {
                text: Theme.t(sheet.step === 2 ? "完成" : "下一步")
                prominent: true
                enabled: sheet.step !== 1 || Store.steamConnected
                onClicked: {
                    if (sheet.step === 2)
                        sheet.close();
                    else
                        sheet.step++;
                }
            }
        }
    }
    onClosed: password.text = ""
}
