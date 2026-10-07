import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

// 已订阅壁纸独立视图：对齐 macOS Components/Workshop/SubscribedWorkshopView.swift。
// 工具栏（筛选/标题/计数/搜索/下载全部/刷新/Steam 状态）与内容状态机
// （加载/未登录/未订阅/无匹配/网格）均对齐上游；下载全部经后端生成
// SubscriptionDownloadPlan 后由确认弹窗呈现（对齐上游 alert）。
Item {
    id: root
    required property var host
    // 窗口较窄时网格内容可能短暂超出容器，裁剪以免与右侧详情重叠。
    clip: true

    // 订阅状态全部由后端 WorkshopViewModel 持有，经 mirage 读取、
    // 经 mirage.setSubscription*/loadSubscriptions 写回（对齐上游
    // workshopViewModel.subscription* 属性）。
    property var subscriptions: mirage.subscriptions
    property bool subscriptionsLoading: mirage.subscriptionsLoading
    property int subscriptionTotal: mirage.subscriptionTotal
    property int subscriptionPage: mirage.subscriptionPage
    property int subscriptionPageCount: mirage.subscriptionPageCount
    property var subscriptionFilters: mirage.subscriptionFilters
    property bool steamReady: mirage.steamReady
    property bool steamLoggedIn: mirage.steamLoggedIn
    property string steamUsername: mirage.steamUsername
    property bool downloadPreparing: mirage.subscriptionDownloadPreparing
    property var downloadPlan: mirage.subscriptionDownloadPlan

    // 首次进入（订阅 tab 打开）且已登录但无缓存内容时加载（对齐 onAppear 分支）。
    Component.onCompleted: {
        if (mirage.steamLoggedIn && subscriptions.length === 0 && !subscriptionsLoading)
            mirage.loadSubscriptions();
    }
    // 登录状态变化时刷新（对齐 onChange(of: steamService.isLoggedIn)）。
    onSteamLoggedInChanged: {
        if (mirage.steamLoggedIn)
            mirage.loadSubscriptions();
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // 工具栏（对齐 macOS SubscribedWorkshopView.toolbar）。
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            FluFilledButton {
                text: qsTr("筛选")
                onClicked: root.host.filtersVisible = !root.host.filtersVisible
            }
            FluText {
                text: qsTr("已订阅")
                font: FluTextStyle.BodyStrong
            }
            FluText {
                visible: root.subscriptions.length > 0
                text: qsTr("共 %1 项").arg(root.subscriptionTotal)
                color: FluTheme.fontSecondaryColor
                font: FluTextStyle.Caption
            }
            FluTextBox {
                Layout.preferredWidth: 200
                Layout.minimumWidth: 140
                Layout.maximumWidth: 240
                placeholderText: qsTr("搜索已订阅壁纸...")
                iconSource: FluentIcons.Search
                text: root.subscriptionFilters.searchText
                onTextChanged: mirage.setSubscriptionSearchText(text)
            }
            Item {
                Layout.fillWidth: true
            }
            // "下载全部"：生成确认计划后由 downloadPlanDialog 呈现
            // （对齐 macOS 的"下载全部"按钮 + alert）。
            FluFilledButton {
                text: root.downloadPreparing ? qsTr("正在准备下载…") : qsTr("下载全部")
                disabled: !root.steamLoggedIn || root.downloadPreparing || root.subscriptions.length === 0
                onClicked: mirage.downloadAllSubscriptions()
            }
            FluIconButton {
                iconSource: FluentIcons.Refresh
                text: qsTr("刷新已订阅壁纸")
                contentDescription: qsTr("刷新已订阅壁纸")
                disabled: !root.steamLoggedIn || root.subscriptionsLoading
                onClicked: mirage.loadSubscriptions()
            }
            // 已订阅与已安装、创意工坊共用图标尺寸偏好；固定 50 项
            // 分页属于数据协议，不由视图菜单或当前窗口尺寸改变。
            WallpaperGridViewMenu {
                explorerIconSize: root.host.explorerIconSize
                onIconSizeChanged: size => root.host.explorerIconSize = size
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
                text: qsTr("登录 Steam")
                onClicked: root.host.openSteamSetup()
            }
        }

        // 独立叠放容器使分页器固定悬浮在内容区底部，不随订阅
        // 网格滚动；滚动内容末尾的 58px 留白避免遮挡最后一行。
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            FluScrollablePage {
                id: subscriptionScrollPage
                anchors.fill: parent
                // 空态铺满视口并保持居中；有数据时恢复由网格
                // contentHeight 驱动的外层滚动模型。
                columnHeight: root.subscriptions.length === 0 ? height : undefined

                // 首次加载（无缓存内容）时显示进度（居中）。
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.subscriptionsLoading && root.subscriptions.length === 0
                    FluProgressRing {
                        anchors.centerIn: parent
                        indeterminate: true
                    }
                }
                // 未登录空态（对齐 !steamService.isLoggedIn 分支，居中显示）。
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: !root.steamLoggedIn
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 8
                        FluIcon {
                            Layout.alignment: Qt.AlignHCenter
                            iconSource: FluentIcons.Contact
                            iconSize: 36
                            iconColor: FluTheme.fontSecondaryColor
                        }
                        FluText {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("登录 Steam 后即可查看已订阅壁纸")
                            font: FluTextStyle.BodyStrong
                        }
                        FluFilledButton {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("登录 Steam")
                            onClicked: root.host.openSteamSetup()
                        }
                    }
                }
                // 空态：尚未订阅任何壁纸（对齐 subscriptionCatalogItems.isEmpty 分支）。
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.steamLoggedIn && !root.subscriptionsLoading && root.subscriptionTotal === 0
                    FluText {
                        anchors.centerIn: parent
                        text: qsTr("尚未订阅任何壁纸")
                        color: FluTheme.fontSecondaryColor
                    }
                }
                // 空态：有订阅记录但当前筛选无匹配（对齐 subscriptionItems.isEmpty 分支）。
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.steamLoggedIn && !root.subscriptionsLoading && root.subscriptionTotal > 0
                            && root.subscriptions.length === 0
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 8
                        FluText {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("没有符合筛选条件的已订阅壁纸")
                            color: FluTheme.fontSecondaryColor
                        }
                        FluFilledButton {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("重置筛选")
                            onClicked: mirage.clearSubscriptionFilters()
                        }
                    }
                }
                // 结果计数（对齐"共 %d 项"）。
                FluText {
                    Layout.alignment: Qt.AlignHCenter
                    visible: root.subscriptions.length > 0
                    text: qsTr("已订阅 %1 项").arg(root.subscriptionTotal)
                    color: FluTheme.fontSecondaryColor
                    font: FluTextStyle.Caption
                }
                // 订阅网格：与浏览页共用 WorkshopItemGrid（滚动模型/列数
                // 自适应/cellHeight 均一致），卡片点击进入详情。
                WorkshopItemGrid {
                    host: root.host
                    items: root.subscriptions
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    visible: root.subscriptionPageCount > 1
                }
            }

            SharedBrowseControls {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                z: 1
                currentPage: root.subscriptionPage
                pageCount: root.subscriptionPageCount
                onSelected: page => mirage.goToSubscriptionPage(page)
                enabled: !root.subscriptionsLoading
                visible: root.subscriptionPageCount > 1
            }
        }
    }

    // 订阅页在本地过滤后按 50 项切片；页码边界变化时重置外层
    // Flickable，使页码按钮、前后翻页和页码输入都从顶部开始。
    onSubscriptionPageChanged: subscriptionScrollPage.resetScroll()

    // 下载全部确认弹窗（对齐 macOS 的 alert：plan.downloadCount 决定按钮文案）。
    FluContentDialog {
        id: downloadPlanDialog
        title: qsTr("下载全部已订阅壁纸")
        message: root.downloadPlanMessage()
        negativeText: qsTr("取消")
        positiveText: root.downloadCount() > 0 ? qsTr("开始下载") : qsTr("好")
        buttonFlags: root.downloadCount() > 0
            ? FluContentDialogType.NegativeButton | FluContentDialogType.PositiveButton
            : FluContentDialogType.PositiveButton
        onNegativeClicked: mirage.dismissSubscriptionDownloadPlan()
        onPositiveClicked: {
            if (root.downloadCount() > 0)
                mirage.confirmSubscriptionDownloads();
            else
                mirage.dismissSubscriptionDownloadPlan();
        }
    }
    // 后端生成/更新计划时弹出确认框。
    onDownloadPlanChanged: {
        if (downloadPlan.subscriptionCount > 0)
            downloadPlanDialog.open();
    }

    function downloadCount() {
        return root.downloadPlan.downloadCount;
    }

    function downloadPlanMessage() {
        var plan = root.downloadPlan;
        if (!(plan.subscriptionCount > 0))
            return "";
        var subscriptionCount = Number(plan.subscriptionCount);
        var remainingCount = Number(plan.remainingCount);
        var downloadCount = Number(plan.downloadCount);
        // 文案对齐 macOS SubscribedWorkshopView 的 alert message。
        if (downloadCount > 0)
            return qsTr("已订阅 %1 个壁纸，还剩 %2 个未下载，本次将下载 %3 个。")
                .arg(subscriptionCount).arg(remainingCount).arg(downloadCount);
        if (remainingCount > 0)
            return qsTr("已订阅 %1 个壁纸，还剩 %2 个正在下载，本次不需要新增下载。")
                .arg(subscriptionCount).arg(remainingCount);
        return qsTr("已订阅 %1 个壁纸，已全部下载，不需要下载。").arg(subscriptionCount);
    }
}
