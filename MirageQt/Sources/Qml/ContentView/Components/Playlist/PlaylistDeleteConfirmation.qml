import QtQuick
import FluentUI
import "../../../GlobalComponents"

// 删除播放列表确认对话框：playlistId/playlistName 由窗口在打开前设置。
MirageDialogWindow {
    id: root
    width: 420
    height: 220
    property string playlistId: ""
    property string playlistName: ""
    title: "删除播放列表"
    message: "确定要删除“" + playlistName + "”吗？"
    negativeText: "取消"
    positiveText: "删除"
    buttonFlags: FluContentDialogType.NegativeButton | FluContentDialogType.PositiveButton
    onPositiveClicked: {
        mirage.deleteSavedPlaylist(root.playlistId);
        root.playlistId = "";
    }
}
