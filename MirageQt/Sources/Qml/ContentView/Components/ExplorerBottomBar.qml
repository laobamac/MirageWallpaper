import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import "Playlist"

ColumnLayout {
    id: bar
    required property var host
    Layout.fillWidth: true
    Layout.preferredHeight: host.currentTab === 0 ? (host.playlistExpanded ? 174 : 44) : 0
    visible: host.currentTab === 0
    clip: true
    spacing: 6

    RowLayout {
        Layout.fillWidth: true
        FluIconButton {
            text: bar.host.playlistExpanded ? "收起播放列表" : "展开播放列表"
            iconSource: bar.host.playlistExpanded ? FluentIcons.ChevronUp : FluentIcons.ChevronDown
            onClicked: bar.host.playlistExpanded = !bar.host.playlistExpanded
        }
        FluText {
            text: "播放列表" + (mirage.playlistItems.length ? " (" + mirage.playlistItems.length + ")" : "")
            font: FluTextStyle.BodyStrong
        }
        FluComboBox {
            Layout.preferredWidth: 114
            model: bar.host.screenLabels()
            currentIndex: mirage.playlistScreen
            onActivated: mirage.playlistScreen = currentIndex
        }
        Item {
            Layout.fillWidth: true
        }
        FluIconButton {
            text: "打开播放列表"
            iconSource: FluentIcons.OpenFile
            display: Button.IconOnly
            onClicked: bar.host.openPlaylistOpenSheet()
        }
        FluIconButton {
            text: "保存播放列表"
            iconSource: FluentIcons.Save
            display: Button.IconOnly
            onClicked: bar.host.openPlaylistSaveSheet()
        }
        FluIconButton {
            text: "播放列表设置"
            iconSource: FluentIcons.Settings
            display: Button.IconOnly
            onClicked: bar.host.openPlaylistSettingsSheet()
        }
        FluIconButton {
            text: "添加所选壁纸"
            iconSource: FluentIcons.Add
            display: Button.IconOnly
            enabled: mirage.selectedWallpaperId.length > 0
            onClicked: mirage.addSelectedToPlaylist()
        }
        FluIconButton {
            text: "清空播放列表"
            iconSource: FluentIcons.Clear
            display: Button.IconOnly
            enabled: mirage.playlistItems.length > 0
            onClicked: mirage.clearPlaylist()
        }
        FluDropDownButton {
            text: "导入壁纸"
            FluMenuItem {
                text: "导入视频文件…"
                onTriggered: bar.host.openImportPanel()
            }
            FluMenuItem {
                text: "导入壁纸文件夹…"
                onTriggered: bar.host.openImportFolderPanel()
            }
        }
    }

    PlaylistStrip {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: bar.host.playlistExpanded
        host: bar.host
    }
}
