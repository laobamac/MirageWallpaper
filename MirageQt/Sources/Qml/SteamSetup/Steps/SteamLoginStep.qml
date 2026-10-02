import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI

ColumnLayout {
    id: root
    property string username: ""
    property string password: ""
    property string guardCode: ""
    property string loginState: mirage.steamLoginState
    property string loginMessage: mirage.steamLoginMessage
    property string guardType: mirage.steamGuardType
    property string qrChallengeUrl: mirage.steamQRCodeUrl
    property bool sessionReusable: mirage.steamSessionReusable
    spacing: 12

    FluText {
        Layout.alignment: Qt.AlignHCenter
        text: qsTr("登录 Steam 账号")
        font: FluTextStyle.Title
    }

    FluText {
        Layout.fillWidth: true
        text: qsTr("需要一个拥有 Wallpaper Engine 的全球 Steam 账号来下载创意工坊内容。")
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        color: FluTheme.fontSecondaryColor
    }

    FluFrame {
        Layout.fillWidth: true
        Layout.leftMargin: 16
        Layout.rightMargin: 16
        Layout.preferredHeight: securityNoticeContent.implicitHeight + 20
        ColumnLayout {
            id: securityNoticeContent
            anchors.fill: parent
            anchors.margins: 10
            spacing: 5
            FluText {
                Layout.fillWidth: true
                text: qsTr("Mirage 并非 Steam 官方客户端。")
                color: Qt.rgba(196 / 255, 121 / 255, 0, 1)
                font: FluTextStyle.BodyStrong
            }
            FluText {
                Layout.fillWidth: true
                text: qsTr("登录通过本机 Steam 服务（SteamKit）安全完成；密码只提交给 Valve 的登录接口，不会写入命令行或 Mirage 日志。会话令牌保存在本机，可随时在创意工坊页面“退出登录”清除。")
                wrapMode: Text.WordWrap
                color: FluTheme.fontSecondaryColor
                font: FluTextStyle.Caption
            }
        }
    }

    FluFrame {
        Layout.fillWidth: true
        Layout.leftMargin: 30
        Layout.rightMargin: 30
        Layout.preferredHeight: savedSessionContent.implicitHeight + 20
        visible: root.loginState !== "success" && root.sessionReusable
        RowLayout {
            id: savedSessionContent
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8
            FluIcon {
                iconSource: FluentIcons.Contact
                iconSize: 20
                iconColor: Qt.rgba(16 / 255, 124 / 255, 16 / 255, 1)
            }
            FluText {
                Layout.fillWidth: true
                text: qsTr("已找到账号 %1 的验证会话。").arg(root.username)
                wrapMode: Text.WordWrap
            }
            FluFilledButton {
                text: qsTr("使用已保存会话")
                onClicked: mirage.useSavedSteamSession()
            }
        }
    }

    // 登录入口：手机扫码（推荐）或密码登录。
    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 70
        Layout.rightMargin: 70
        visible: root.loginState === "idle" || root.loginState === "failed"
        spacing: 8

        FluFilledButton {
            Layout.fillWidth: true
            text: qsTr("使用 Steam 手机应用扫码登录")
            onClicked: mirage.loginSteamQR()
        }
        FluText {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("— 或使用密码登录 —")
            color: FluTheme.fontSecondaryColor
            font: FluTextStyle.Caption
        }
        FluTextBox {
            Layout.fillWidth: true
            placeholderText: qsTr("全球 Steam 登录账户名（非昵称）")
            iconSource: FluentIcons.Contact
            text: root.username
            onTextChanged: root.username = text
        }
        FluPasswordBox {
            Layout.fillWidth: true
            placeholderText: qsTr("密码")
            text: root.password
            onTextChanged: root.password = text
        }
        FluFilledButton {
            Layout.fillWidth: true
            text: qsTr("登录")
            enabled: root.username.trim().length > 0 && root.password.length > 0
            onClicked: mirage.loginSteam(root.username, root.password)
        }
    }

    // QR 等待：显示 Steam 挑战二维码（FluQRCode，对齐上游 qrView 的
    // CIFilter.qrCodeGenerator 展示），并提供刷新/复制/取消。
    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 70
        Layout.rightMargin: 70
        visible: root.loginState === "waitingForQR"
        spacing: 10
        // 编码内容即 SteamServiceManager 提供的挑战 URL；黑码白底与上游一致。
        FluQRCode {
            Layout.alignment: Qt.AlignHCenter
            text: root.qrChallengeUrl
            size: 196
            visible: root.qrChallengeUrl.length > 0
        }
        FluText {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("使用 Steam 手机应用扫描二维码")
            font: FluTextStyle.BodyStrong
        }
        FluText {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("在 Steam 手机应用中打开扫码功能并确认登录")
            color: FluTheme.fontSecondaryColor
            font: FluTextStyle.Caption
        }
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 8
            FluButton {
                text: qsTr("刷新二维码")
                onClicked: mirage.loginSteamQR()
            }
            FluButton {
                text: qsTr("复制链接")
                enabled: root.qrChallengeUrl.length > 0
                onClicked: {
                    // 链接即挑战授权，复制后可在手机浏览器打开完成确认。
                    mirage.copyTextToClipboard(root.qrChallengeUrl)
                }
            }
            FluButton {
                text: qsTr("取消")
                onClicked: mirage.cancelSteamLogin()
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignHCenter
        visible: root.loginState === "loggingIn"
        spacing: 10
        FluProgressRing { Layout.alignment: Qt.AlignHCenter }
        FluText {
            Layout.alignment: Qt.AlignHCenter
            text: root.loginMessage.length > 0 ? root.loginMessage : qsTr("正在登录...")
            color: FluTheme.fontSecondaryColor
        }
        FluButton {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("取消登录")
            onClicked: mirage.cancelSteamLogin()
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 70
        Layout.rightMargin: 70
        visible: root.loginState === "waitingForGuard" && root.guardType !== "mobileConfirm"
        spacing: 8
        FluIcon {
            Layout.alignment: Qt.AlignHCenter
            iconSource: root.guardType === "email" ? FluentIcons.Mail : FluentIcons.MobileTablet
            iconSize: 34
            iconColor: FluTheme.primaryColor
        }
        FluText {
            Layout.alignment: Qt.AlignHCenter
            text: root.guardType === "email" ? qsTr("请输入邮箱验证码") : qsTr("请输入手机验证码")
            font: FluTextStyle.BodyStrong
        }
        FluTextBox {
            Layout.fillWidth: true
            placeholderText: qsTr("Steam Guard 验证码")
            iconSource: FluentIcons.Lock
            text: root.guardCode
            onTextChanged: root.guardCode = text
        }
        FluFilledButton {
            Layout.fillWidth: true
            text: qsTr("验证")
            enabled: root.guardCode.trim().length > 0
            onClicked: mirage.submitSteamGuardCode(root.guardCode)
        }
        FluButton {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("取消登录")
            onClicked: mirage.cancelSteamLogin()
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 70
        Layout.rightMargin: 70
        visible: root.loginState === "waitingForGuard" && root.guardType === "mobileConfirm"
        spacing: 10
        FluIcon {
            Layout.alignment: Qt.AlignHCenter
            iconSource: FluentIcons.MobileTablet
            iconSize: 42
            iconColor: FluTheme.primaryColor
        }
        FluText {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("请在手机上确认登录")
            font: FluTextStyle.BodyStrong
        }
        FluText {
            Layout.fillWidth: true
            text: root.loginMessage
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            color: FluTheme.fontSecondaryColor
        }
        FluButton {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("取消登录")
            onClicked: mirage.cancelSteamLogin()
        }
    }

    FluFrame {
        Layout.fillWidth: true
        Layout.leftMargin: 70
        Layout.rightMargin: 70
        Layout.preferredHeight: loginSuccessContent.implicitHeight + 24
        visible: root.loginState === "success"
        RowLayout {
            id: loginSuccessContent
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10
            FluIcon {
                iconSource: FluentIcons.CheckMark
                iconSize: 28
                iconColor: Qt.rgba(16 / 255, 124 / 255, 16 / 255, 1)
            }
            ColumnLayout {
                Layout.fillWidth: true
                FluText { text: qsTr("Steam 登录完成"); font: FluTextStyle.BodyStrong }
                FluText {
                    Layout.fillWidth: true
                    text: qsTr("登录账号：%1").arg(root.username)
                    color: FluTheme.fontSecondaryColor
                    elide: Text.ElideRight
                }
            }
            FluButton {
                text: qsTr("退出登录")
                onClicked: mirage.logoutSteam()
            }
        }
    }

    FluText {
        Layout.fillWidth: true
        visible: root.loginMessage.length > 0 && root.loginState === "failed"
        text: root.loginMessage
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        color: Qt.rgba(196 / 255, 43 / 255, 28 / 255, 1)
    }
}
