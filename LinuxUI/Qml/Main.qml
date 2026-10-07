// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    width: 1300
    height: 640
    minimumWidth: 1300
    minimumHeight: 640
    visible: true
    title: "Mirage 1.0.0"
    color: Theme.window
    font.pixelSize: 13
    palette.window: Theme.window
    palette.windowText: Theme.text
    palette.base: Theme.field
    palette.text: Theme.text
    palette.button: Theme.panel
    palette.buttonText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: "white"
    property var onlineItem: ({})
    property bool showingCreator: false
    property var creator: ({})
    property string dialogKind: ""
    property string dialogValue: ""
    function openImport() {
        folderPicker.open();
    }
    function openSettings() {
        settings.present(0);
    }
    function showDialog(kind) {
        if (kind === "author") {
            creator = Store.tab === 0 ? Store.selected : onlineItem;
            showingCreator = true;
            return;
        }
        dialogKind = kind;
        dialogValue = "";
        genericSheet.open();
    }
    Connections {
        target: bridge
        function onError(message) {
            Store.notice = message;
        }
    }
    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal
        handle: Rectangle {
            implicitWidth: 1
            color: Theme.line
        }
        ColumnLayout {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 640
            spacing: 5
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 16
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Rectangle {
                        implicitWidth: tabs.implicitWidth + 8
                        implicitHeight: 42
                        radius: 21
                        color: Theme.dark ? "#343437" : "#dfdfe3"
                        RowLayout {
                            id: tabs
                            anchors.centerIn: parent
                            spacing: 4
                            Repeater {
                                model: [
                                    {
                                        text: "已安装",
                                        icon: "download"
                                    },
                                    {
                                        text: "发现",
                                        icon: "sparkle"
                                    },
                                    {
                                        text: "创意工坊",
                                        icon: "cloud"
                                    },
                                    {
                                        text: "已订阅",
                                        icon: "check"
                                    }
                                ]
                                MButton {
                                    required property var modelData
                                    required property int index
                                    text: Theme.t(modelData.text)
                                    symbol: modelData.icon
                                    implicitHeight: 34
                                    leftPadding: 14
                                    rightPadding: 14
                                    implicitWidth: labelWidth + 36
                                    property real labelWidth: fontMetrics.advanceWidth(text) + 20
                                    font.weight: Font.DemiBold
                                    prominent: Store.tab === index
                                    flatStyle: true
                                    pill: true
                                    onClicked: Store.tab = index
                                }
                            }
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        implicitWidth: chrome.implicitWidth + 6
                        implicitHeight: 36
                        radius: 18
                        color: Theme.dark ? "#303033" : "#e4e4e7"
                        RowLayout {
                            id: chrome
                            anchors.centerIn: parent
                            spacing: 2
                            MButton {
                                text: Theme.t("移动端")
                                symbol: "phone"
                                flatStyle: true
                                enabled: false
                            }
                            MButton {
                                text: bridge.screens.length === 1 ? bridge.screens[0].name : Theme.t("显示器") + " " + (Store.display + 1)
                                symbol: "display"
                                flatStyle: true
                                onClicked: displayMenu.popup()
                            }
                            MButton {
                                text: Theme.t("设置")
                                symbol: "settings"
                                flatStyle: true
                                onClicked: settings.present(0)
                            }
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 64
                    radius: 8
                    color: Theme.dark ? "#3a3329" : "#faf0df"
                    border.color: Theme.dark ? "#856331" : "#edcc96"
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 10
                        Icon {
                            name: "bubble"
                            color: Theme.orange
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            MText {
                                text: Theme.t("Mirage 仍处于早期阶段")
                                font.bold: true
                            }
                            MText {
                                text: Theme.t("遇到问题请认真撰写 Issue，或加入 QQ 交流群 2160040437 反馈。")
                                font.pixelSize: 11
                                color: Theme.secondary
                                Layout.fillWidth: true
                            }
                        }
                        MButton {
                            text: Theme.t("支持 Mirage")
                            symbol: "heart"
                            onClicked: settings.present(4)
                        }
                        MButton {
                            text: Theme.t("提交 Issue")
                            symbol: "edit"
                            onClicked: Qt.openUrlExternally("https://github.com/laobamac/MirageWallpaper/issues/new/choose")
                        }
                        MButton {
                            id: copyGroup
                            text: Theme.t("复制群号")
                            symbol: "copy"
                            prominent: true
                            onClicked: {
                                bridge.copyText("2160040437");
                                text = Theme.t("群号已复制");
                                copiedTimer.restart();
                            }
                        }
                    }
                }
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: Store.tab
                    LibraryView {
                        onImportRequested: importMenu.popup()
                        onContextRequested: itemMenu.popup()
                    }
                    OnlineView {
                        section: 1
                        onSettingsRequested: settings.present(1)
                        onLoginRequested: steam.open()
                        onDownloadRequested: window.showDialog("downloads")
                        onItemRequested: item => window.onlineItem = item
                    }
                    OnlineView {
                        section: 2
                        onSettingsRequested: settings.present(1)
                        onLoginRequested: steam.open()
                        onDownloadRequested: window.showDialog("downloads")
                        onItemRequested: item => window.onlineItem = item
                    }
                    OnlineView {
                        section: 3
                        onSettingsRequested: settings.present(1)
                        onLoginRequested: steam.open()
                        onDownloadRequested: window.showDialog("downloads")
                        onItemRequested: item => window.onlineItem = item
                    }
                }
                PlaylistPane {
                    visible: Store.tab === 0
                    Layout.fillWidth: true
                    onImportRequested: importMenu.popup()
                    onDialogRequested: kind => {
                        if (kind === "playlistSettings")
                            playlistSettings.open();
                        else
                            window.showDialog(kind);
                    }
                }
            }
        }
        Item {
            SplitView.preferredWidth: window.showingCreator ? 420 : 340
            SplitView.minimumWidth: 320
            SplitView.maximumWidth: 420
            DetailPane {
                anchors.fill: parent
                visible: Store.tab === 0 && !window.showingCreator
                onCloseRequested: window.close()
                onMetadataRequested: window.showDialog("metadata")
                onContextRequested: window.showDialog("author")
            }
            CreatorProfile {
                anchors.fill: parent
                visible: window.showingCreator
                author: window.creator
                onBack: window.showingCreator = false
                onSelected: wallpaper => {
                    Store.selectedId = wallpaper.id;
                    window.showingCreator = false;
                    Store.tab = 0;
                }
            }
            WorkshopDetail {
                anchors.fill: parent
                visible: Store.tab !== 0 && !window.showingCreator
                item: window.onlineItem
                onAuthorRequested: author => {
                    window.onlineItem = author;
                    window.showDialog("author");
                }
            }
        }
    }
    FontMetrics {
        id: fontMetrics
        font: window.font
    }
    Timer {
        id: copiedTimer
        interval: 1800
        onTriggered: copyGroup.text = Theme.t("复制群号")
    }
    SettingsWindow {
        id: settings
        transientParent: window
        onLoginRequested: steam.open()
    }
    WelcomeSheet {
        id: welcome
    }
    SteamSheet {
        id: steam
    }
    BakeSheet {
        id: bake
    }
    PlaylistSettings {
        id: playlistSettings
    }
    FolderDialog {
        id: folderPicker
        title: Theme.t("选择壁纸目录")
        onAccepted: bridge.addSource(selectedFolder)
    }
    FileDialog {
        id: videoPicker
        title: Theme.t("导入视频")
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Video (*.mp4 *.webm *.mkv *.mov *.m4v)"]
        onAccepted: {
            for (const url of selectedFiles)
                bridge.addSource(url);
        }
    }
    Menu {
        id: importMenu
        MenuItem {
            text: Theme.t("从文件夹导入…")
            onTriggered: folderPicker.open()
        }
        MenuItem {
            text: Theme.t("导入视频…")
            onTriggered: videoPicker.open()
        }
    }
    Menu {
        id: itemMenu
        MenuItem {
            text: Theme.t("设为壁纸")
            enabled: Store.rendererConnected && !!Store.selected.id
            onTriggered: Store.apply()
        }
        MenuItem {
            text: Theme.t("设为屏保")
            enabled: false
        }
        MenuItem {
            text: Theme.t("烘焙为视频")
            enabled: !!Store.selected.id
            onTriggered: bake.open()
        }
        MenuItem {
            text: Theme.t("烘焙任务")
            onTriggered: window.showDialog("bakeTasks")
        }
        MenuItem {
            text: Theme.t("设为动态锁屏")
            enabled: false
        }
        MenuSeparator {}
        MenuItem {
            text: Theme.t("加入播放列表")
            enabled: !!Store.selected.id
            onTriggered: Store.addToPlaylist(Store.selectedId)
        }
        MenuItem {
            text: Theme.t(Store.favorites.includes(Store.selectedId) ? "取消收藏" : "加入收藏")
            enabled: !!Store.selected.id
            onTriggered: Store.favorite(Store.selectedId)
        }
        MenuItem {
            text: Theme.t("删除壁纸")
            enabled: !!Store.selected.id
            onTriggered: window.showDialog("delete")
        }
        MenuSeparator {}
        MenuItem {
            text: Theme.t("在创意工坊中打开")
            enabled: /^\d+$/.test(String(Store.selected.workshopid || ""))
            onTriggered: Qt.openUrlExternally("https://steamcommunity.com/sharedfiles/filedetails/?id=" + Store.selected.workshopid)
        }
        MenuItem {
            text: Theme.t("查看作者主页和作品")
            enabled: !!Store.selected.author
            onTriggered: window.showDialog("author")
        }
        Menu {
            title: Theme.t("相关壁纸")
            enabled: false
            MenuItem {
                text: Theme.t("浏览该作者全部")
            }
            MenuItem {
                text: Theme.t("浏览预设")
            }
        }
        Menu {
            title: Theme.t("举报与屏蔽")
            enabled: false
            MenuItem {
                text: Theme.t("举报")
            }
            MenuItem {
                text: Theme.t("管理屏蔽列表")
            }
        }
        MenuSeparator {}
        MenuItem {
            text: Theme.t("设置快捷键")
            enabled: !!Store.selected.id
            onTriggered: window.showDialog("shortcut")
        }
        MenuItem {
            text: Theme.t("在文件管理器中显示")
            enabled: !!Store.selected.path
            onTriggered: bridge.openFolder(Store.selected.path)
        }
    }
    Menu {
        id: displayMenu
        Repeater {
            model: bridge.screens
            MenuItem {
                required property var modelData
                required property int index
                text: Theme.t("显示器") + " " + (index + 1) + " · " + modelData.name + " · " + modelData.width + " × " + modelData.height
                checkable: true
                checked: Store.display === index
                onTriggered: Store.display = index
            }
        }
        MenuSeparator {}
        MenuItem {
            text: Theme.t("停止此显示器")
            enabled: Store.rendererConnected
            onTriggered: Store.request("stop", {
                display: Store.display
            })
        }
        MenuItem {
            text: Theme.t("全部停止")
            enabled: Store.rendererConnected
            onTriggered: Store.request("stopAll", {})
        }
    }
    Sheet {
        id: genericSheet
        width: window.dialogKind === "author" ? 680 : 500
        contentItem: ColumnLayout {
            spacing: 18
            MText {
                text: Theme.t(({
                        playlistOpen: "载入播放列表",
                        playlistSave: "保存播放列表",
                        playlistClear: "清空播放列表",
                        metadata: "编辑壁纸信息",
                        delete: "删除壁纸",
                        shortcut: "设置快捷键",
                        downloads: "下载",
                        bakeTasks: "烘焙任务",
                        author: "作者主页"
                    })[window.dialogKind] || "")
                font.pixelSize: 20
                font.bold: true
            }
            MText {
                visible: window.dialogKind === "playlistClear"
                text: Theme.t("确定要清空当前播放列表中的全部壁纸吗？")
                Layout.fillWidth: true
            }
            MField {
                visible: window.dialogKind === "playlistSave"
                Layout.fillWidth: true
                placeholderText: Theme.t("播放列表名称")
                text: window.dialogValue
                onTextEdited: window.dialogValue = text
            }
            SavedPlaylists {
                visible: window.dialogKind === "playlistOpen"
                onLoaded: genericSheet.close()
            }
            ColumnLayout {
                visible: window.dialogKind === "metadata"
                Layout.fillWidth: true
                MText {
                    text: Theme.t("壁纸名称")
                }
                MField {
                    id: metadataTitle
                    Layout.fillWidth: true
                    text: Store.selectedRuntime.title || Store.selected.title || ""
                }
                MText {
                    text: Theme.t("标签")
                }
                MField {
                    id: metadataTags
                    Layout.fillWidth: true
                    text: (Store.selectedRuntime.tags || Store.selected.tags || []).join(", ")
                }
                MText {
                    text: Theme.t("修改保存在 Mirage 配置中，不改动原壁纸文件。")
                    font.pixelSize: 11
                    color: Theme.secondary
                    Layout.fillWidth: true
                }
            }
            MText {
                visible: window.dialogKind === "delete"
                text: Theme.t("移除壁纸目录只会从 Mirage 壁纸库中移除，不会删除原文件。")
                Layout.fillWidth: true
            }
            MField {
                visible: window.dialogKind === "shortcut"
                Layout.fillWidth: true
                placeholderText: Theme.t("按下快捷键")
                readOnly: true
                Keys.onPressed: event => {
                    text = (event.modifiers & Qt.ControlModifier ? "Ctrl+" : "") + (event.modifiers & Qt.AltModifier ? "Alt+" : "") + (event.modifiers & Qt.ShiftModifier ? "Shift+" : "") + event.text.toUpperCase();
                    window.dialogValue = text;
                    event.accepted = true;
                }
            }
            TaskManager {
                visible: ["downloads", "bakeTasks"].includes(window.dialogKind)
                baking: window.dialogKind === "bakeTasks"
                Layout.fillWidth: true
            }
            RowLayout {
                Item {
                    Layout.fillWidth: true
                }
                MButton {
                    text: Theme.t("取消")
                    onClicked: genericSheet.close()
                }
                MButton {
                    text: Theme.t(window.dialogKind === "playlistClear" ? "清空" : "好")
                    prominent: true
                    enabled: window.dialogKind !== "playlistSave" || window.dialogValue.trim() !== ""
                    onClicked: {
                        if (window.dialogKind === "playlistSave")
                            Store.savePlaylist(window.dialogValue.trim());
                        if (window.dialogKind === "playlistClear")
                            Store.clearPlaylist();
                        if (window.dialogKind === "metadata") {
                            Store.setRuntime("title", metadataTitle.text);
                            Store.setRuntime("tags", metadataTags.text.split(",").map(x => x.trim()).filter(x => x));
                        }
                        if (window.dialogKind === "delete")
                            bridge.hideWallpaper(Store.selectedId);
                        if (window.dialogKind === "shortcut")
                            Store.setRuntime("shortcut", window.dialogValue);
                        genericSheet.close();
                    }
                }
            }
        }
    }
    Sheet {
        id: errorSheet
        width: 480
        contentItem: ColumnLayout {
            spacing: 16
            MText {
                text: Store.notice
                Layout.fillWidth: true
            }
            MButton {
                text: Theme.t("好")
                prominent: true
                Layout.alignment: Qt.AlignRight
                onClicked: {
                    Store.notice = "";
                    errorSheet.close();
                }
            }
        }
    }
    Connections {
        target: Store
        function onNoticeChanged() {
            if (Store.notice)
                errorSheet.open();
        }
    }
    Shortcut {
        sequence: "Ctrl+,"
        onActivated: settings.present(0)
    }
    Shortcut {
        sequence: "Ctrl+O"
        onActivated: folderPicker.open()
    }
    Shortcut {
        sequence: "Ctrl+Q"
        onActivated: Qt.quit()
    }
    Component.onCompleted: if (!bridge.load("welcomeDismissed", false))
        welcome.open()
}
