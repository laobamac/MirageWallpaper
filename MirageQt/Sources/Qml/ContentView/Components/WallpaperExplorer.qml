import QtQuick
import QtQuick.Layouts
import FluentUI
import "../ContentViewLogic.js" as ContentViewLogic

// 已安装壁纸保留自身的强业务卡片，仅与两个工坊页共享自适应
// 列宽和分页器协议。滚动区与底部分页器分离，保证分页器不随内容滚动。
Item {
    id: root
    required property var host

    FluScrollablePage {
        id: wallpaperScrollPage
        anchors.fill: parent

        GridView {
            id: wallpaperGrid
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            interactive: false
            clip: true
            model: root.host.pagedWallpapers
            // 自适应列宽对齐 macOS GridItem(.adaptive)；图标尺寸只改变
            // 列数与卡片尺寸，不参与固定 50 项的分页计算。
            cellWidth: ContentViewLogic.adaptiveGridCellWidth(
                width, root.host.explorerIconSize, 14)
            cellHeight: cellWidth

            // 已安装卡片需要壁纸选中、播放与右键业务，不与
            // WorkshopItemCard 合并为接收任意模型的通用 delegate。
            delegate: InstalledWallpaperCard {
                required property var modelData
                host: root.host
                itemData: modelData
                width: wallpaperGrid.cellWidth - 14
                height: wallpaperGrid.cellHeight - 14
            }
        }

        FluText {
            Layout.fillWidth: true
            Layout.topMargin: 24
            visible: wallpaperGrid.count === 0
            text: qsTr("没有找到匹配的壁纸。")
            horizontalAlignment: Text.AlignHCenter
            color: FluTheme.fontSecondaryColor
        }

        // 多页时在滚动内容末尾保留分页器高度，避免最后一行
        // 卡片被底部悬浮层遮挡；单页不产生额外留白。
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            visible: root.host.wallpaperPageCount > 1
        }
    }

    SharedBrowseControls {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        z: 1
        visible: root.host.wallpaperPageCount > 1
        currentPage: root.host.wallpaperCurrentPage
        pageCount: root.host.wallpaperPageCount
        onSelected: page => root.host.setWallpaperPage(page)
    }

    // 页码变化时由外层 Flickable 回到顶部；GridView 本身禁止交互，
    // 因此不能仅调整 GridView 的内部位置。
    Connections {
        target: root.host
        function onWallpaperCurrentPageChanged() {
            wallpaperScrollPage.resetScroll();
        }
    }
}
