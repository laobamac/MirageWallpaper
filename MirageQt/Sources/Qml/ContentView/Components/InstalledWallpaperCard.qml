import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../GlobalComponents"

// 已安装壁纸卡片：共享组件将标题与元数据叠在封面渐变上。本文件仅提供
// 本地壁纸字段及其左键、右键和双击语义。
WallpaperItemCard {
    id: root

    // 选中判定：已安装壁纸选中态由 mirage.selectedWallpaperId 驱动。
    property bool selected: String(itemData.id) === String(mirage.selectedWallpaperId)

    // 本地记录只有 title、typeLabel 和 favorite 三项已定义数据；以类别和
    // 收藏状态填充封面下沿，既保持信息密度也不凭空构造工坊统计字段。
    FluText {
        Layout.fillWidth: true
        text: String(root.itemData.title)
        elide: Text.ElideRight
        font: FluTextStyle.BodyStrong
        color: "white"
    }
    RowLayout {
        Layout.fillWidth: true
        FluText {
            Layout.fillWidth: true
            text: String(root.itemData.typeLabel)
            elide: Text.ElideRight
            color: Qt.rgba(1, 1, 1, 0.90)
            font: FluTextStyle.Caption
        }
        FluIcon {
            visible: Boolean(root.itemData.favorite)
            iconSource: FluentIcons.HeartFill
            iconSize: 14
            iconColor: Qt.rgba(1, 1, 1, 0.92)
        }
    }

    onClicked: function(mouse) {
        if (mouse.button === Qt.RightButton) {
            // 右键：先选中再弹上下文菜单（对齐旧 delegate 行为）。
            mirage.selectWallpaper(String(root.itemData.id));
            root.host.openWallpaperContextMenu(root.itemData);
            return;
        }
        mirage.selectWallpaper(String(root.itemData.id));
    }
    onDoubleClicked: function(mouse) {
        if (mouse.button !== Qt.LeftButton)
            return;
        // 应用壁纸前对网页壁纸执行信任确认（对齐旧 delegate 的
        // runWithWallpaperTrust 包装）。
        root.host.runWithWallpaperTrust(root.itemData, function() {
            mirage.applyWallpaper(String(root.itemData.id), false);
        });
    }
}
