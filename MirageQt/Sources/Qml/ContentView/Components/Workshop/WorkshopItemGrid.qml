import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../ContentViewLogic.js" as ContentViewLogic

// 创意工坊壁纸网格：WorkshopView（浏览）与 SubscribedWorkshopView（已订阅）
// 共用的网格配置（对齐 macOS ScrollView + LazyVGrid 滚动模型）。滚动模型要点：
// - Layout.preferredHeight: contentHeight + interactive: false：网格按内容
//   展开，滚动交给外层 FluScrollablePage（GridView 默认 implicitHeight 为
//   0，fillHeight 无剩余空间分配会把网格压成 0 高导致项目不可见）；
// - cellWidth 走 adaptiveGridCellWidth（对齐 macOS GridItem(.adaptive(
//   minimum:explorerIconSize, maximum:2×explorerIconSize), spacing:14)）：
//   随 explorerIconSize 变化（订阅页视图菜单可调），间隔统一 14。
// 发现分区（横向 ListView）与已安装（方形网格）为不同布局协议，不共用本组件。
GridView {
    id: root

    required property var host
    // 网格数据源（浏览页 workshopItems / 订阅页 subscriptions）。
    required property var items

    Layout.fillWidth: true
    Layout.preferredHeight: contentHeight
    interactive: false
    clip: true
    model: root.items
    cellWidth: ContentViewLogic.adaptiveGridCellWidth(width, root.host.explorerIconSize, 14)
    // 卡片视觉协议为正方形；cellWidth 中已含 14px 网格间隔，所以高度也
    // 使用同一值，delegate 保留的 14px 形成各方向一致的留白。
    cellHeight: cellWidth
    delegate: WorkshopItemCard {
        required property var modelData
        host: root.host
        itemData: modelData
        width: Math.min(root.cellWidth, Math.max(164, root.cellWidth - 14))
        height: width
    }
}
