import QtQuick
import FluentUI
import "../../../GlobalComponents"

// 删除导入壁纸确认对话框：确认后删除选中壁纸及其文件（不可恢复）。
MirageDialogWindow {
    id: root
    width: 420
    height: 220
    title: "删除导入壁纸"
    message: "确定要删除“" + (mirage.selectedWallpaper.title || "") + "”及其所有文件吗？此操作不可恢复。"
    negativeText: "取消"
    positiveText: "删除"
    buttonFlags: FluContentDialogType.NegativeButton | FluContentDialogType.PositiveButton
    onPositiveClicked: mirage.deleteSelectedWallpaper()
}
