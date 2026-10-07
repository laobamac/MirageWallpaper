import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

ColumnLayout {
    id: root

    required property var host
    property var item: mirage.selectedWorkshopItem
    // selectedWorkshopItem uses the fixed workshopItemMap() field protocol.
    // An empty map represents no selection and keeps the existing empty state.
    property string itemId: item.id
    property string state: item.downloadState
    property bool installed: item.downloaded
    property bool needsDependency: item.needsDependency
    property bool active: item.downloadActive
        || ["queued", "starting", "connecting", "downloading", "resolving"].indexOf(root.state) >= 0
    property double progress: item.downloadProgress

    spacing: 12

    function workshopUrl() {
        return "https://steamcommunity.com/sharedfiles/filedetails/?id=" + root.itemId;
    }

    function openDownloadDirectory() {
        mirage.revealWorkshopDownload(root.itemId);
    }

    // 内容直接在详情层 FluScrollablePage 中展开滚动，
    // 不再嵌套滚动容器，避免 FluPage implicitHeight 塌缩。
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12
        visible: root.itemId.length > 0

        WorkshopImage {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.min(280, Math.max(180, root.width - 30))
            Layout.preferredHeight: Layout.preferredWidth
            imageUrl: root.item.preview
            contentMode: Image.PreserveAspectCrop
            // 规则 2：GIF 只在真实选中（selectedWorkshopItem 非空）时播放；
            // 未选中时 itemId 为空串，isAnimating: false 显示静态首帧。
            isAnimating: root.itemId.length > 0
        }

        FluText {
            Layout.fillWidth: true
            text: root.item.title
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font: FluTextStyle.Subtitle
        }

        FluButton {
            Layout.fillWidth: true
            visible: root.itemId.length > 0 && root.item.creatorSteamId.length > 0
            text: qsTr("查看作者的其他创意工坊作品")
            onClicked: {
                Qt.openUrlExternally("https://steamcommunity.com/profiles/"
                                     + root.item.creatorSteamId + "/myworkshopfiles/");
            }
        }

        FluFrame {
            Layout.fillWidth: true
            Layout.preferredHeight: presetNoticeContent.implicitHeight + 18
            visible: root.item.type === "preset"
            RowLayout {
                id: presetNoticeContent
                anchors.fill: parent
                anchors.margins: 9
                spacing: 8
                FluIcon {
                    iconSource: FluentIcons.SliderThumb
                    iconSize: 18
                    iconColor: Qt.rgba(120 / 255, 70 / 255, 160 / 255, 1)
                }
                FluText {
                    Layout.fillWidth: true
                    text: qsTr("创意工坊预设可能需要基础壁纸。")
                    wrapMode: Text.WordWrap
                    color: FluTheme.fontSecondaryColor
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Repeater {
                model: [
                    [FluentIcons.Download, root.item.subscriptions, qsTr("订阅")],
                    [FluentIcons.HeartFill, root.item.favorited, qsTr("收藏")],
                    [FluentIcons.RedEye, root.item.views, qsTr("浏览")]
                ]
                delegate: FluFrame {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 5
                        spacing: 2
                        FluIcon {
                            Layout.alignment: Qt.AlignHCenter
                            iconSource: modelData[0]
                            iconSize: 15
                            iconColor: FluTheme.primaryColor
                        }
                        FluText {
                            Layout.alignment: Qt.AlignHCenter
                            text: modelData[1] + " " + modelData[2]
                            font: FluTextStyle.Caption
                            color: FluTheme.fontSecondaryColor
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            FluText {
                text: root.item.typeLabel
                color: FluTheme.fontSecondaryColor
            }
            FluText {
                Layout.fillWidth: true
                text: root.item.sizeLabel
                color: FluTheme.fontSecondaryColor
            }
            FluText {
                visible: root.itemId.length > 0 && root.item.rating.length > 0
                text: root.item.rating
                color: root.item.rating === "Mature"
                    ? Qt.rgba(196 / 255, 43 / 255, 28 / 255, 1)
                    : Qt.rgba(196 / 255, 121 / 255, 0, 1)
            }
        }

        FluDivider { Layout.fillWidth: true }
        FluText { text: qsTr("标签"); font: FluTextStyle.BodyStrong }
        Flow {
            Layout.fillWidth: true
            spacing: 5
            visible: root.itemId.length > 0 && root.item.tags.length > 0
            Repeater {
                model: root.item.tags
                delegate: FluFrame {
                    required property var modelData
                    implicitWidth: tagText.implicitWidth + 14
                    implicitHeight: 24
                    radius: 3
                    FluText {
                        id: tagText
                        anchors.centerIn: parent
                        text: String(modelData)
                        color: FluTheme.fontSecondaryColor
                        font: FluTextStyle.Caption
                    }
                }
            }
        }

        FluText {
            Layout.fillWidth: true
            visible: root.itemId.length > 0 && root.item.tags.length === 0
            text: qsTr("无标签")
            color: FluTheme.fontTertiaryColor
        }

        FluDivider { Layout.fillWidth: true }
        FluText { text: qsTr("描述"); font: FluTextStyle.BodyStrong }
        FluText {
            Layout.fillWidth: true
            text: root.itemId.length > 0 && root.item.description.length > 0
                ? root.item.description : qsTr("无描述")
            wrapMode: Text.WordWrap
            maximumLineCount: 8
            elide: Text.ElideRight
            color: FluTheme.fontSecondaryColor
        }

        FluDivider { Layout.fillWidth: true }
        FluText { text: qsTr("操作"); font: FluTextStyle.BodyStrong }

        FluFrame {
            Layout.fillWidth: true
            Layout.preferredHeight: dependencyNoticeContent.implicitHeight + 18
            visible: root.installed && root.needsDependency
            RowLayout {
                id: dependencyNoticeContent
                anchors.fill: parent
                anchors.margins: 9
                spacing: 8
                FluIcon {
                    iconSource: FluentIcons.Warning
                    iconSize: 18
                    iconColor: Qt.rgba(196 / 255, 121 / 255, 0, 1)
                }
                FluText {
                    Layout.fillWidth: true
                    text: qsTr("此预设已安装，但缺少基础壁纸。")
                    wrapMode: Text.WordWrap
                    color: FluTheme.fontSecondaryColor
                }
                FluButton {
                    text: qsTr("下载基础壁纸")
                    onClicked: {
                        mirage.requestWorkshopPresetDependency(root.itemId);
                    }
                }
            }
        }

        FluText {
            Layout.fillWidth: true
            visible: root.active
            text: {
                if (root.state === "queued") return qsTr("等待 Steam 服务按顺序下载…");
                if (root.state === "starting" || root.state === "connecting")
                    return qsTr("正在连接 Steam 服务…");
                if (root.state === "resolving") return qsTr("正在处理下载...");
                if (root.state === "downloading") return root.progress < 0
                    ? qsTr("正在连接 Steam...")
                    : qsTr("正在下载 (%1%)").arg(Math.round(root.progress));
                return root.item.downloadMessage;
            }
            color: FluTheme.fontSecondaryColor
        }
        FluProgressBar {
            Layout.fillWidth: true
            visible: root.active
            indeterminate: root.progress < 0 || root.state === "starting"
                || root.state === "connecting" || root.state === "resolving"
            from: 0
            to: 1
            value: root.progress / 100
        }
        FluButton {
            Layout.fillWidth: true
            visible: root.state === "failed"
            text: qsTr("重试下载")
            onClicked: mirage.retryWorkshopDownload(root.itemId)
        }
        FluFilledButton {
            Layout.fillWidth: true
            visible: !root.installed && !root.active && root.state !== "failed"
                && root.itemId.length > 0 && mirage.steamReady
            text: root.needsDependency ? qsTr("下载基础壁纸") : qsTr("下载壁纸")
            onClicked: mirage.downloadWorkshopItem(root.itemId)
        }
        FluButton {
            Layout.fillWidth: true
            visible: !root.installed && !root.active && root.state !== "failed"
                && root.itemId.length > 0 && !mirage.steamReady
            text: qsTr("设置 Steam 后下载")
            onClicked: root.host.openSteamSetup()
        }
        FluButton {
            Layout.fillWidth: true
            visible: root.active
            text: qsTr("取消下载")
            onClicked: mirage.cancelWorkshopDownload(root.itemId)
        }
        FluButton {
            Layout.fillWidth: true
            visible: root.installed && !root.active
            text: qsTr("打开下载目录")
            onClicked: root.openDownloadDirectory()
        }
        FluButton {
            Layout.fillWidth: true
            visible: root.itemId.length > 0
            text: qsTr("在 Steam 中查看")
            onClicked: Qt.openUrlExternally(root.workshopUrl())
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.itemId.length > 0 && mirage.steamLoggedIn
            spacing: 8
            FluFilledButton {
                Layout.fillWidth: true
                text: qsTr("订阅")
                onClicked: mirage.subscribeWorkshopItem(root.itemId)
            }
            FluButton {
                Layout.fillWidth: true
                text: qsTr("取消订阅")
                onClicked: mirage.unsubscribeWorkshopItem(root.itemId)
            }
        }

        FluDivider { Layout.fillWidth: true }
        FluText {
            Layout.fillWidth: true
            text: qsTr("创意工坊 ID：%1").arg(root.itemId)
            color: FluTheme.fontTertiaryColor
            font: FluTextStyle.Caption
            elide: Text.ElideRight
        }
        FluText {
            Layout.fillWidth: true
            visible: root.itemId.length > 0 && root.item.updatedAt.length > 0
            text: qsTr("更新于：%1").arg(root.item.updatedAt)
            color: FluTheme.fontTertiaryColor
            font: FluTextStyle.Caption
        }
    }

    // Qt 布局特性：嵌套 ColumnLayout 即使设置 Layout.fillWidth 也不会
    // 拉伸超过自身隐式宽度（实测宽度仍=内容宽度），导致内部 AlignHCenter
    // 无空间可居中（"只会随侧栏左边移动"）。普通 Item 作为 fillWidth
    // 子项可正常拉伸，故用 Item 撑满、内部 ColumnLayout anchors.fill 布局，
    // 空状态内容即可在详情区宽度内水平垂直居中。
    Item {
        id: emptyState
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: root.itemId.length === 0

        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            Item { Layout.fillHeight: true }
            FluIcon {
                Layout.alignment: Qt.AlignHCenter
                iconSource: FluentIcons.PreviewLink
                iconSize: 34
                iconColor: FluTheme.fontTertiaryColor
            }
            FluText {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("选择创意工坊壁纸以查看详情")
                color: FluTheme.fontSecondaryColor
            }
            Item { Layout.fillHeight: true }
        }
    }
}
