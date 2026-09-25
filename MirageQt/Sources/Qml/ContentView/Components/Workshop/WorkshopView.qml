import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

// 创意工坊浏览视图：对齐 macOS Components/Workshop/WorkshopView.swift 的
// 浏览模式（订阅已提升为独立 tab，见 SubscribedWorkshopView.qml）。
Item {
    id: root
    required property var host
    // 窗口较窄时网格内容可能短暂超出容器，裁剪以免与右侧详情重叠。
    clip: true

    property var items: mirage.workshopItems
    property bool loading: mirage.workshopLoading
    property string errorText: mirage.workshopError
    property int page: mirage.workshopPage
    property int pageCount: mirage.workshopPageCount
    property bool steamReady: mirage.steamReady
    property string steamSummary: mirage.steamSetupSummary
    property int activeDownloads: mirage.activeDownloadCount
    property var downloadQueue: mirage.downloadQueue
    property bool steamLoggedIn: mirage.steamLoggedIn
    property string steamUsername: mirage.steamUsername

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            FluFilledButton {
                text: qsTr("筛选")
                onClicked: root.host.filtersVisible = !root.host.filtersVisible
            }
            FluTextBox {
                Layout.preferredWidth: 200
                Layout.minimumWidth: 140
                Layout.maximumWidth: 240
                placeholderText: qsTr("搜索作品、作者或作品 ID…")
                iconSource: FluentIcons.Search
                text: root.host.workshopSearchText
                onTextChanged: {
                    root.host.workshopSearchText = text;
                    mirage.setWorkshopSearchText(text);
                }
                onCommit: mirage.submitWorkshopSearch()
            }
            Item { Layout.fillWidth: true }
            FluIconButton {
                iconSource: FluentIcons.Refresh
                text: qsTr("刷新创意工坊")
                contentDescription: qsTr("刷新创意工坊")
                disabled: root.loading
                onClicked: mirage.submitWorkshopSearch()
            }
            // 创意工坊与已安装、已订阅共用同一图标尺寸偏好；
            // 该偏好只影响自适应列宽，不改变服务端固定 50 项的页容量。
            WallpaperGridViewMenu {
                explorerIconSize: root.host.explorerIconSize
                onIconSizeChanged: size => root.host.explorerIconSize = size
            }
            FluComboBox {
                Layout.preferredWidth: 130
                Layout.minimumWidth: 110
                model: root.host.workshopSortOptions.map(function(option) { return option.label; })
                currentIndex: root.host.workshopSortIndex()
                onActivated: {
                    root.host.workshopSortKey = root.host.workshopSortOptions[currentIndex].key;
                    mirage.setWorkshopSortOrder(root.host.workshopSortKey);
                }
            }
            FluIconButton {
                id: downloadButton
                iconSource: FluentIcons.Download
                text: qsTr("下载管理")
                contentDescription: qsTr("下载管理")
                onClicked: {
                    if (downloadPopover.opened)
                        downloadPopover.close();
                    else
                        downloadPopover.openFor(downloadButton);
                }
                // FIXME: Badge 会太上面了，然后会被裁剪掉，先注释
                // FluBadge {
                //     position: "topRight"
                //     count: root.activeDownloads
                //     visible: root.activeDownloads > 0
                // }
            }
            RowLayout {
                visible: root.steamLoggedIn
                spacing: 4
                FluIcon {
                    iconSource: FluentIcons.ContactSolid
                    iconSize: 15
                    iconColor: Qt.rgba(16 / 255, 124 / 255, 16 / 255, 1)
                }
                FluText {
                    text: root.steamUsername
                    elide: Text.ElideRight
                    Layout.maximumWidth: 80
                    color: FluTheme.fontSecondaryColor
                    font: FluTextStyle.Caption
                }
                FluIconButton {
                    iconSource: FluentIcons.SignOut
                    text: qsTr("退出 Steam")
                    contentDescription: qsTr("退出 Steam")
                    onClicked: mirage.logoutSteam()
                }
            }
            FluFilledButton {
                visible: !root.steamLoggedIn
                text: qsTr("设置 Steam")
                onClicked: root.host.openSteamSetup()
            }
        }

        FluFrame {
            id: steamSetupBanner
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            visible: !root.steamReady
            radius: 8
            color: FluTheme.dark
                ? Qt.rgba(0.08, 0.32, 0.55, 0.18)
                : Qt.rgba(0.0, 0.47, 0.84, 0.08)
            border.color: FluTheme.dark
                ? Qt.rgba(0.20, 0.55, 0.85, 0.32)
                : Qt.rgba(0.0, 0.47, 0.84, 0.20)
            RowLayout {
                id: steamSetupBannerContent
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12
                FluIcon {
                    iconSource: FluentIcons.CloudDownload
                    iconSize: 21
                    iconColor: FluTheme.primaryColor
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    FluText {
                        text: qsTr("连接 Steam 以下载壁纸")
                        font: FluTextStyle.BodyStrong
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: root.steamSummary.length > 0 ? root.steamSummary : qsTr("需要连接 Steam 服务")
                        wrapMode: Text.WordWrap
                        color: FluTheme.fontSecondaryColor
                    }
                }
                FluFilledButton {
                    text: qsTr("设置 Steam")
                    onClicked: root.host.openSteamSetup()
                }
            }
        }

        // 内容滚动区和分页器使用同一叠放容器：分页器固定在底部，
        // 网格末尾预留 58px 保证最后一行可完整滚动到其上方。
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            FluScrollablePage {
                id: workshopScrollPage
                anchors.fill: parent
                // 空态铺满视口以保持居中；有数据时由网格 contentHeight
                // 决定滚动内容高度，不让内层 GridView 自行滚动。
                columnHeight: root.items.length === 0 ? height : undefined

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.loading && root.items.length === 0
                    FluProgressRing {
                        anchors.centerIn: parent
                        indeterminate: true
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: !root.loading && root.items.length === 0
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 8
                        FluText {
                            Layout.alignment: Qt.AlignHCenter
                            text: root.errorText.length > 0
                                ? root.errorText : qsTr("没有找到壁纸")
                            color: root.errorText.length > 0
                                ? Qt.rgba(196 / 255, 43 / 255, 28 / 255, 1)
                                : FluTheme.fontSecondaryColor
                        }
                        FluButton {
                            Layout.alignment: Qt.AlignHCenter
                            visible: root.errorText.length > 0
                            text: qsTr("重试")
                            onClicked: mirage.submitWorkshopSearch()
                        }
                    }
                }

                WorkshopItemGrid {
                    visible: root.items.length > 0
                    host: root.host
                    items: root.items
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    visible: root.pageCount > 1
                }
            }

            SharedBrowseControls {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                z: 1
                currentPage: root.page
                pageCount: root.pageCount
                onSelected: page => mirage.goToWorkshopPage(page)
                enabled: !root.loading
                visible: root.pageCount > 1
            }
        }

    }

    // 服务端页码改变后回到外层滚动区顶部，使页码按钮、前后翻页
    // 和页码输入保持相同交互。
    onPageChanged: workshopScrollPage.resetScroll()

    DownloadPopover {
        id: downloadPopover
        tasks: root.downloadQueue
    }
}
