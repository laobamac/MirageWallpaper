// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import "../Resources/qrcode.js" as QrCodeLib

Sheet {
    id: sheet
    width: 620
    property int step: 0
    property bool passwordVisible: false
    property int loginMode: 0
    property string qrPath: ""
    property string qrError: ""
    // Generates Steam's challenge QR as vector geometry, keeping it crisp when the sheet scales.
    // Signature: updateQrCode(text: string) -> void; QRCode.createPath reports capacity errors by throwing.
    function updateQrCode(text) {
        try {
            qrPath = QrCodeLib.QRCode.createPath(text);
            qrError = "";
        } catch (error) {
            qrPath = "";
            qrError = Theme.t("登录二维码内容过长");
        }
    }
    Connections {
        target: controller
        // Signature: onSteamChanged() -> void; refreshes the QR when the service publishes a challenge URL.
        function onSteamChanged() { sheet.updateQrCode(controller.steamQRCodeUrl); }
    }
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
                        onClicked: {
                            sheet.loginMode = 1;
                            controller.loginSteamQR();
                        }
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
                        enabled: !controller.steamLoggedIn && username.text !== "" && password.text !== ""
                        onClicked: controller.loginSteam(username.text, password.text)
                    }
                }
                EmptyState {
                    visible: sheet.loginMode === 1 && controller.steamQRCodeUrl === ""
                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    symbol: "grid"
                    title: Theme.t(controller.steamLoginState === "idle" ? "等待登录二维码" : controller.steamLoginState)
                    subtitle: controller.steamLoginMessage || controller.steamSetupSummary
                }
                Rectangle {
                    visible: sheet.loginMode === 1 && qrPath !== ""
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 272
                    Layout.preferredHeight: 272
                    color: "white"
                    Shape {
                        anchors.centerIn: parent
                        width: 256
                        height: 256
                        ShapePath {
                            fillColor: "#111820"
                            strokeColor: "transparent"
                            PathSvg { path: sheet.qrPath }
                        }
                    }
                    Accessible.name: Theme.t("Steam 登录二维码")
                }
                MText {
                    visible: sheet.qrError !== ""
                    text: sheet.qrError
                    color: Theme.red
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                }
                MField {
                    id: qrChallenge
                    visible: sheet.loginMode === 1 && controller.steamQRCodeUrl !== ""
                    Layout.fillWidth: true
                    readOnly: true
                    text: controller.steamQRCodeUrl
                    Accessible.name: Theme.t("Steam 登录挑战链接")
                }
                RowLayout {
                    visible: qrChallenge.visible
                    Layout.fillWidth: true
                    MButton {
                        text: Theme.t("复制登录链接")
                        symbol: "copy"
                        onClicked: bridge.copyText(controller.steamQRCodeUrl)
                    }
                    MButton {
                        text: Theme.t("刷新二维码")
                        symbol: "refresh"
                        onClicked: controller.loginSteamQR()
                    }
                    MButton {
                        text: Theme.t("取消登录")
                        onClicked: controller.cancelSteamLogin()
                    }
                }
                MField {
                    id: guardCode
                    visible: controller.steamGuardType !== ""
                    Layout.fillWidth: true
                    placeholderText: Theme.t("Steam Guard 验证码")
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: guardCode.visible
                    MButton {
                        text: Theme.t("提交验证码")
                        prominent: true
                        enabled: guardCode.text !== ""
                        onClicked: {
                            controller.submitSteamGuardCode(guardCode.text);
                            guardCode.text = "";
                        }
                    }
                    MButton {
                        text: Theme.t("取消登录")
                        onClicked: controller.cancelSteamLogin()
                    }
                }
                MText {
                    text: controller.steamLoginMessage || controller.steamSetupSummary
                    visible: text !== ""
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.secondary
                }
                MButton {
                    text: Theme.t("使用已保存的 Steam 会话")
                    visible: controller.steamSessionReusable && !controller.steamLoggedIn
                    Layout.fillWidth: true
                    onClicked: controller.useSavedSteamSession()
                }
                MButton {
                    text: Theme.t("退出 Steam")
                    visible: controller.steamLoggedIn
                    Layout.fillWidth: true
                    onClicked: controller.logoutSteam()
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
    Component.onCompleted: updateQrCode(controller.steamQRCodeUrl)
    onClosed: password.text = ""
}
