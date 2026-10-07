import QtQuick
import FluentUI

// 已安装壁纸右键菜单：对应 macOS ContextMenus/ExplorerItemMenu.swift。
// 通过 host 访问窗口（打开播放列表），通过 mirage 全局对象操作数据；
// 未实现功能（屏保/Steam 查看/取消订阅）以信号通知窗口弹出 TODO 提示。
FluMenu {
    id: menu
    required property var host
    property var wallpaper: ({})
    signal unavailableFeatureRequested
    signal deleteRequested
    width: Math.min(240, Math.max(220, host.width - 32))

    FluMenuItem {
        text: "设为屏保（TODO）"
        iconSource: FluentIcons.SettingsDisplaySound
        onTriggered: menu.unavailableFeatureRequested()
    }
    FluMenuSeparator {}
    FluMenu {
        title: "加入播放列表"
        Repeater {
            model: mirage.screenCount
            delegate: FluMenuItem {
                required property int index
                text: "显示器 " + (index + 1)
                iconSource: FluentIcons.Add
                onTriggered: menu.host.addWallpaperToPlaylist(menu.wallpaper.id, index)
            }
        }
    }
    FluMenuItem {
        text: menu.wallpaper.favorite ? "取消收藏" : "加入收藏"
        iconSource: menu.wallpaper.favorite ? FluentIcons.HeartFill : FluentIcons.Heart
        onTriggered: {
            mirage.selectWallpaper(menu.wallpaper.id);
            mirage.toggleSelectedFavorite();
        }
    }
    FluMenuSeparator {}
    FluMenuItem {
        text: "在文件管理器中显示"
        iconSource: FluentIcons.FolderOpen
        onTriggered: Qt.openUrlExternally(menu.wallpaper.location)
    }
    FluMenuItem {
        text: "在 Steam 中查看（TODO）"
        iconSource: FluentIcons.OpenFile
        visible: menu.wallpaper.source === "workshop"
        onTriggered: menu.unavailableFeatureRequested()
    }
    FluMenuSeparator {}
    FluMenuItem {
        text: "删除导入壁纸"
        iconSource: FluentIcons.Delete
        visible: menu.wallpaper.source === "imported"
        onTriggered: {
            mirage.selectWallpaper(menu.wallpaper.id);
            menu.deleteRequested();
        }
    }
    FluMenuItem {
        text: "取消订阅（TODO）"
        iconSource: FluentIcons.Delete
        visible: menu.wallpaper.source === "workshop"
        onTriggered: menu.unavailableFeatureRequested()
    }
}
