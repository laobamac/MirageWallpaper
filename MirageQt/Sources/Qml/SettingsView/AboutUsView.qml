import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI

// 关于页：对齐 macOS SettingsView/AboutUsView.swift 的内容，
// 排版遵循 FluentUI example（T_Settings）惯例：FluScrollablePage 直接
// 作根、各段用 Layout.fillWidth 布局、内容按自身隐式高度撑开。
// 注意：容器不要用「无显式高度的 Rectangle + 内部 anchors.fill」——Rectangle
// 的 implicitHeight 为 0，内容会塌缩不可见；带背景的段用
// Item{implicitHeight: 内容高度} + Rectangle 平铺背景。
FluScrollablePage {
    id: root
    required property var host

    property string usdtAddress: "0xFc0a5C52e3A085FEc7b077FE3D2C413114Bf880D"
    property bool copiedUSDT: false
    // 当前放大的 QR id（"" 表示无），与 sponsorCards 的 id 对应。
    property string enlargedId: ""

    // 赞助卡片数据（对齐上游 SponsorQRModel / afdianURL）：
    // openExternally=true 的卡片点击打开外部链接，否则点击放大。
    property var sponsorCards: [
        { id: "afdian", source: "qrc:/sponsorship/afdian.jpg", title: "爱发电", subtitle: "点击打开爱发电", openExternally: true, url: "https://www.ifdian.net/a/laobamac" },
        { id: "wechat-pay", source: "qrc:/sponsorship/wechat-pay.png", title: "微信支付", subtitle: "使用微信扫一扫", openExternally: false, url: "" },
        { id: "alipay", source: "qrc:/sponsorship/alipay.jpg", title: "支付宝", subtitle: "使用支付宝扫一扫", openExternally: false, url: "" }
    ]

    function openSponsor(card) {
        if (card.openExternally)
            Qt.openUrlExternally(card.url);
        else
            root.enlargedId = card.id;
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 24

        // ---- 头部：应用图标 + 标题（左对齐）----
        RowLayout {
            Layout.fillWidth: true
            spacing: 20

            Image {
                Layout.preferredWidth: 88
                Layout.preferredHeight: 88
                source: "qrc:/appicon.png"
                sourceSize.width: 88
                sourceSize.height: 88
                fillMode: Image.PreserveAspectFit
            }
            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 90
                color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.15)
            }
            ColumnLayout {
                spacing: 6
                Layout.alignment: Qt.AlignVCenter
                FluText {
                    text: "Mirage"
                    font: FluTextStyle.Title
                }
                FluText {
                    text: "Linux 动态壁纸引擎"
                    color: FluTheme.fontSecondaryColor
                    font: FluTextStyle.Caption
                }
                FluText {
                    text: "场景 · 网页 · 视频"
                    color: FluTheme.fontTertiaryColor
                    font: FluTextStyle.Caption
                }
            }
        }

        // ---- 版本信息（左对齐）----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            FluText {
                text: "版本 1.0.0（开发构建）"
                color: FluTheme.fontSecondaryColor
            }
            RowLayout {
                spacing: 4
                FluText { text: "作者" }
                FluText {
                    text: "王孝慈 (laobamac)"
                    font: FluTextStyle.BodyStrong
                }
            }
            FluTextButton {
                text: "github.com/laobamac/MirageWallpaper"
                onClicked: Qt.openUrlExternally("https://github.com/laobamac/MirageWallpaper")
            }
        }

        // ---- 赞助 section（对齐 macOS sponsorSection：粉底圆角卡片）----
        // 带背景的段：用 Item 的 implicitHeight 撑起内容高度，Rectangle 平铺
        // 背景（直接 Rectangle + anchors.fill 会因 implicitHeight=0 塌缩）。
        Item {
            Layout.fillWidth: true
            implicitHeight: sponsorColumn.implicitHeight + 32

            Rectangle {
                anchors.fill: parent
                radius: 12
                color: Qt.rgba(1, 0.41, 0.71, 0.08)
                border.color: Qt.rgba(1, 0.41, 0.71, 0.25)
            }
            ColumnLayout {
                id: sponsorColumn
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                FluText {
                    text: "支持 Mirage"
                    font: FluTextStyle.BodyStrong
                }
                FluText {
                    Layout.fillWidth: true
                    text: "Mirage 会继续免费开放开发。若它为你的桌面带来了价值，欢迎按自己的意愿赞助；每一份支持都会用于持续维护与兼容性改进。"
                    wrapMode: Text.WordWrap
                    color: FluTheme.fontSecondaryColor
                }
                // 三张 QR 卡 + USDT（对齐 macOS HStack(alignment: .top)）。
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    // 三张 QR 卡（爱发电外链 + 微信/支付宝可放大），
                    // 结构同构用 Repeater 数据驱动渲染。
                    Repeater {
                        model: root.sponsorCards
                        delegate: Item {
                            required property var modelData
                            Layout.preferredWidth: 118
                            // Item 无显式高度时隐式高度为 0，需按内容撑开。
                            implicitHeight: cardColumn.implicitHeight

                            ColumnLayout {
                                id: cardColumn
                                width: parent.width
                                spacing: 6
                                Image {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 118
                                    Layout.preferredHeight: 154
                                    source: modelData.source
                                    fillMode: Image.PreserveAspectFit
                                    layer.smooth: true
                                }
                                FluText {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: modelData.title
                                    font: FluTextStyle.BodyStrong
                                }
                                FluText {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: modelData.subtitle
                                    color: FluTheme.fontSecondaryColor
                                    font: FluTextStyle.Caption
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.openSponsor(modelData)
                            }
                        }
                    }

                    // USDT 卡片（对齐 macOS usdtCard：VStack leading + 顶部对齐）。
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 6
                        FluText {
                            text: "USDT"
                            font: FluTextStyle.BodyStrong
                        }
                        FluText {
                            text: "海外赞助"
                            color: FluTheme.fontSecondaryColor
                            font: FluTextStyle.Caption
                        }
                        FluText {
                            text: "接收 USDT"
                            color: FluTheme.fontSecondaryColor
                            font: FluTextStyle.Caption
                        }
                        FluText {
                            Layout.fillWidth: true
                            text: root.usdtAddress
                            wrapMode: Text.WrapAnywhere
                            color: FluTheme.fontSecondaryColor
                            font: FluTextStyle.Caption
                        }
                        FluButton {
                            text: root.copiedUSDT ? "地址已复制" : "复制地址"
                            onClicked: {
                                mirage.copyTextToClipboard(root.usdtAddress);
                                root.copiedUSDT = true;
                            }
                        }
                    }
                }
            }
        }
    }

    // 放大预览：半透明遮罩 + 大 QR + 标题，点击任意处关闭（对齐上游 enlargedOverlay）。
    Popup {
        id: enlargePopup
        parent: root
        anchors.centerIn: parent
        modal: true
        focus: true
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
        padding: 0
        background: null

        // 打开/关闭的缩放 + 淡入动画（对齐 macOS spring(response:0.42) 的放大效果）。
        enter: Transition {
            NumberAnimation {
                property: "scale"
                from: 0.6
                to: 1.0
                duration: 220
                easing.type: Easing.OutCubic
            }
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: 220
            }
        }
        exit: Transition {
            NumberAnimation {
                property: "scale"
                from: 1.0
                to: 0.85
                duration: 160
                easing.type: Easing.InCubic
            }
            NumberAnimation {
                property: "opacity"
                from: 1
                to: 0
                duration: 160
            }
        }

        contentItem: Rectangle {
            width: 340
            height: 400
            radius: 16
            color: FluTheme.dark ? Qt.rgba(0.1, 0.1, 0.12, 0.96) : Qt.rgba(1, 1, 1, 0.96)
            border.color: FluTheme.dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.12)

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 14

                Image {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 300
                    Layout.preferredHeight: 300
                    source: root.enlargedSource()
                    fillMode: Image.PreserveAspectFit
                    layer.smooth: true
                }
                FluText {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.enlargedTitle()
                    font: FluTextStyle.Subtitle
                }
                FluText {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.enlargedSubtitle()
                    color: FluTheme.fontSecondaryColor
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: enlargePopup.close()
        }
    }

    function enlargedSource() {
        for (var i = 0; i < root.sponsorCards.length; ++i) {
            if (root.sponsorCards[i].id === root.enlargedId)
                return root.sponsorCards[i].source;
        }
        return "";
    }

    function enlargedTitle() {
        for (var i = 0; i < root.sponsorCards.length; ++i) {
            if (root.sponsorCards[i].id === root.enlargedId)
                return root.sponsorCards[i].title;
        }
        return "";
    }

    function enlargedSubtitle() {
        for (var i = 0; i < root.sponsorCards.length; ++i) {
            if (root.sponsorCards[i].id === root.enlargedId)
                return root.sponsorCards[i].subtitle;
        }
        return "";
    }

    onEnlargedIdChanged: {
        if (root.enlargedId.length > 0)
            enlargePopup.open();
    }
    Connections {
        target: enlargePopup
        function onClosed() { root.enlargedId = ""; }
    }
}
