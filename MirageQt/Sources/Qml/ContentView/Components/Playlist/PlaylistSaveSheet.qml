import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

MirageDialogWindow {
    id: root
    required property var host
    title: "保存播放列表"
    width: 380
    height: 260
    negativeText: "取消"
    positiveText: "保存"
    buttonFlags: FluContentDialogType.NegativeButton | FluContentDialogType.PositiveButton
    onOpened: root.host.playlistSaveName = ""
    onPositiveClicked: {
            var name = root.host.playlistSaveName.trim();
            if (name.length > 0)
                mirage.savePlaylist(name);
        }
        contentDelegate: Component {
            ColumnLayout {
                width: parent.width
                spacing: 8
                FluText {
                    text: "名称"
                }
                FluTextBox {
                    Layout.fillWidth: true
                    placeholderText: "播放列表名称"
                    text: root.host.playlistSaveName
                    onTextChanged: root.host.playlistSaveName = text
                    onCommit: {
                        var name = text.trim();
                        if (name.length > 0) {
                            mirage.savePlaylist(name);
                            root.close();
                        }
                    }
                }
                FluText {
                    Layout.fillWidth: true
                    text: "同名播放列表将被覆盖。"
                    wrapMode: Text.WordWrap
                }
            }
    }
}
