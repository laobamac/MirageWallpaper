import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import FluentUI
import "Steps"

FluWindow {
    id: setup

    SteamSetupViewModel {
        id: setupModel
    }

    property alias currentStep: setupModel.currentStep
    property alias username: setupModel.username
    property alias password: setupModel.password
    property alias guardCode: setupModel.guardCode
    property alias canProceed: setupModel.canProceed

    width: 520
    height: 560
    minimumWidth: 520
    minimumHeight: 560
    title: qsTr("Steam 创意工坊设置")
    autoDestroy: false
    fixSize: true
    showMinimize: false
    showMaximize: false

    onVisibleChanged: {
        if (visible)
            setupModel.refreshFromService();
        else
            setupModel.cancelPendingWork();
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 12
            Layout.bottomMargin: 14
            spacing: 4

            Repeater {
                model: 3
                delegate: Item {
                    required property int index
                    Layout.fillWidth: index < 2
                    Layout.preferredWidth: 28
                    implicitHeight: 44

                    RowLayout {
                        anchors.fill: parent
                        spacing: 5

                        FluFrame {
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            radius: 14
                            color: index <= setup.currentStep
                                ? FluTheme.primaryColor : FluTheme.frameColor
                            FluIcon {
                                anchors.centerIn: parent
                                iconSource: index < setup.currentStep
                                    ? FluentIcons.CheckMark : 0
                                iconSize: 14
                                iconColor: "white"
                                visible: index < setup.currentStep
                            }
                            FluText {
                                anchors.centerIn: parent
                                visible: index >= setup.currentStep
                                text: index + 1
                                color: index === setup.currentStep
                                    ? "white" : FluTheme.fontSecondaryColor
                                font: FluTextStyle.Caption
                            }
                        }
                        FluRectangle {
                            Layout.preferredHeight: 2
                            Layout.fillWidth: true
                            visible: index < 2
                            color: index < setup.currentStep
                                ? FluTheme.primaryColor : FluTheme.dividerColor
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: index <= setup.currentStep && !setupModel.busy
                        onClicked: setup.currentStep = index
                    }
                }
            }
        }

        FluDivider {
            Layout.fillWidth: true
        }

        FluScrollablePage {
            id: bodyScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 10
            Layout.bottomMargin: 10

            StackLayout {
                id: bodyStack
                Layout.fillWidth: true
                currentIndex: setup.currentStep

                ColumnLayout {
                    spacing: 20
                    Layout.fillWidth: true

                    Item { Layout.fillHeight: true }
                    FluIcon {
                        Layout.alignment: Qt.AlignHCenter
                        iconSource: FluentIcons.CloudDownload
                        iconSize: 58
                        iconColor: FluTheme.primaryColor
                    }
                    FluText {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("设置 Steam 创意工坊")
                        font: FluTextStyle.Title
                    }
                    FluText {
                        Layout.fillWidth: true
                        Layout.leftMargin: 40
                        Layout.rightMargin: 40
                        text: qsTr("连接 Steam 创意工坊，浏览并下载数十万 Wallpaper Engine 壁纸。")
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        color: FluTheme.fontSecondaryColor
                    }
                    FluFrame {
                        id: ownershipFrame
                        Layout.fillWidth: true
                        Layout.leftMargin: 40
                        Layout.rightMargin: 40
                        Layout.preferredHeight: ownershipContent.implicitHeight + 20
                        radius: 8
                        color: FluTheme.dark
                            ? Qt.rgba(0.45, 0.25, 0.05, 0.22)
                            : Qt.rgba(1.0, 0.75, 0.20, 0.13)
                        border.color: FluTheme.dark
                            ? Qt.rgba(0.90, 0.58, 0.12, 0.35)
                            : Qt.rgba(0.78, 0.45, 0.0, 0.20)
                        RowLayout {
                            id: ownershipContent
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 8
                            FluIcon {
                                iconSource: FluentIcons.Warning
                                iconSize: 18
                                iconColor: Qt.rgba(196 / 255, 121 / 255, 0, 1)
                            }
                            FluText {
                                Layout.fillWidth: true
                                text: qsTr("需要在 Steam 上拥有 Wallpaper Engine 才能下载创意工坊壁纸")
                                wrapMode: Text.WordWrap
                                color: FluTheme.fontSecondaryColor
                            }
                        }
                    }
                    FluFrame {
                        Layout.fillWidth: true
                        Layout.leftMargin: 40
                        Layout.rightMargin: 40
                        Layout.preferredHeight: networkContent.implicitHeight + 20
                        radius: 8
                        color: FluTools.withOpacity(FluTheme.fontSecondaryColor, 0.08)
                        border.width: 0
                        ColumnLayout {
                            id: networkContent
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 5
                            RowLayout {
                                spacing: 6
                                FluIcon {
                                    iconSource: FluentIcons.InternetSharing
                                    iconSize: 16
                                    iconColor: FluTheme.fontSecondaryColor
                                }
                                FluText {
                                    text: qsTr("中国大陆网络说明")
                                    color: FluTheme.fontSecondaryColor
                                    font: FluTextStyle.Caption
                                }
                            }
                            FluText {
                                Layout.fillWidth: true
                                text: qsTr("此功能依赖全球 Steam 的 Web API、Steam 登录服务和内容 CDN。蒸汽平台兼容性不保证；网络线路只能改善个别环节，Mirage 不承诺任何线路一定能解决登录或下载问题。")
                                wrapMode: Text.WordWrap
                                color: FluTheme.fontSecondaryColor
                                font: FluTextStyle.Caption
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 50
                        Layout.rightMargin: 50
                        spacing: 14
                        Repeater {
                            model: [
                                [FluentIcons.Search, qsTr("搜索浏览"), qsTr("搜索、筛选、排序海量壁纸")],
                                [FluentIcons.Download, qsTr("一键下载"), qsTr("通过 Steam 服务直接下载到本地")],
                                [FluentIcons.BrushSize, qsTr("即刻使用"), qsTr("下载完成自动加入壁纸库")]
                            ]
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 12
                                Item {
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 32
                                    FluIcon {
                                        anchors.centerIn: parent
                                        iconSource: modelData[0]
                                        iconSize: 21
                                        iconColor: FluTheme.primaryColor
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    FluText {
                                        text: modelData[1]
                                        font: FluTextStyle.BodyStrong
                                    }
                                    FluText {
                                        Layout.fillWidth: true
                                        text: modelData[2]
                                        wrapMode: Text.WordWrap
                                        color: FluTheme.fontSecondaryColor
                                        font: FluTextStyle.Caption
                                    }
                                }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }

                SteamLoginStep {
                    Layout.fillWidth: true
                    username: setupModel.username
                    password: setupModel.password
                    guardCode: setupModel.guardCode
                    onUsernameChanged: setupModel.username = username
                    onPasswordChanged: setupModel.password = password
                    onGuardCodeChanged: setupModel.guardCode = guardCode
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 14
                    Item { Layout.fillHeight: true }
                    FluIcon {
                        Layout.alignment: Qt.AlignHCenter
                        iconSource: FluentIcons.CheckMark
                        iconSize: 60
                        iconColor: Qt.rgba(16 / 255, 124 / 255, 16 / 255, 1)
                    }
                    FluText {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("设置完成！")
                        font: FluTextStyle.Title
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: qsTr("Steam 登录已完成。Wallpaper Engine 所有权与项目访问权限将在首次下载时由 Steam 验证。")
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        color: FluTheme.fontSecondaryColor
                    }
                    FluFrame {
                        id: accountFrame
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: Math.min(420, parent.width - 36)
                        Layout.preferredHeight: accountContent.implicitHeight + 24
                        RowLayout {
                            id: accountContent
                            anchors.fill: parent
                            anchors.margins: 12
                            FluIcon { iconSource: FluentIcons.Contact; iconSize: 18; iconColor: FluTheme.primaryColor }
                            FluText {
                                Layout.fillWidth: true
                                text: setupModel.username.length > 0
                                    ? qsTr("账号：%1").arg(setupModel.username)
                                    : qsTr("Steam 账号已准备就绪")
                                elide: Text.ElideRight
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        FluDivider {
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 12
            spacing: 8
            FluButton {
                visible: setup.currentStep > 0
                text: qsTr("上一步")
                enabled: !setupModel.busy
                onClicked: setupModel.previousStep()
            }
            Item { Layout.fillWidth: true }
            FluFilledButton {
                text: setup.currentStep === 2 ? qsTr("完成") : qsTr("下一步")
                // Automatic session restoration may still be running while the
                // welcome page is shown; it must not block entering the login
                // step. The login-step guard remains enforced by canProceed.
                enabled: setup.currentStep === 0
                         || (setup.canProceed && !setupModel.busy)
                onClicked: {
                    if (setup.currentStep === 2) {
                        setupModel.completeSetup();
                        setup.close();
                    } else {
                        setupModel.nextStep();
                    }
                }
            }
        }
    }
}
