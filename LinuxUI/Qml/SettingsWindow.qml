// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Window {
    id: settings
    title: Theme.t("设置")
    width: 780
    height: 620
    minimumWidth: 720
    minimumHeight: 520
    color: Theme.panel
    modality: Qt.NonModal
    property int page: 0
    property string snapshot: ""
    property bool committed: false
    readonly property bool dirty: snapshot !== JSON.stringify(Store.preferences)
    signal loginRequested
    function present(index) {
        page = index;
        snapshot = JSON.stringify(Store.preferences);
        committed = false;
        show();
        raise();
        requestActivate();
    }
    onClosing: {
        if (!committed && snapshot) {
            Store.preferences = JSON.parse(snapshot);
            bridge.language = Store.preferences.language;
        }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 12
            Layout.bottomMargin: 8
            spacing: 4
            Repeater {
                model: [
                    {
                        title: "性能",
                        icon: "gauge"
                    },
                    {
                        title: "通用",
                        icon: "settings"
                    },
                    {
                        title: "插件",
                        icon: "grid"
                    },
                    {
                        title: "屏保",
                        icon: "display"
                    },
                    {
                        title: "关于",
                        icon: "person"
                    }
                ]
                Rectangle {
                    required property var modelData
                    required property int index
                    Layout.preferredWidth: 74
                    Layout.preferredHeight: sixty
                    property int sixty: 60
                    radius: 6
                    color: settings.page === index ? (Theme.dark ? "#243b55" : "#d8e8fc") : tabHover.hovered ? Theme.hover : "transparent"
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 4
                        Icon {
                            name: modelData.icon
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 24
                            Layout.alignment: Qt.AlignHCenter
                            color: settings.page === index ? Theme.accent : Theme.text
                        }
                        MText {
                            text: Theme.t(modelData.title)
                            font.pixelSize: 11
                            color: settings.page === index ? Theme.accent : Theme.text
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                    HoverHandler {
                        id: tabHover
                    }
                    TapHandler {
                        onTapped: settings.page = index
                    }
                    Accessible.role: Accessible.PageTab
                    Accessible.name: Theme.t(modelData.title)
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.line
        }
        ScrollView {
            id: settingsScroll
            Layout.fillHeight: true
            Layout.fillWidth: true
            clip: true
            ColumnLayout {
                width: settingsScroll.availableWidth
                spacing: 20
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: 20
                    spacing: 20
                    ColumnLayout {
                        visible: settings.page === 0
                        Layout.fillWidth: true
                        spacing: 20
                        FormSection {
                            title: "播放"
                            symbol: "play"
                            SettingRow {
                                label: "其他应用获得焦点时"
                                setting: "focus"
                                choices: ["保持运行", "静音", "暂停"]
                            }
                            SettingRow {
                                label: "窗口覆盖屏幕超过阈值时暂停"
                                setting: "coverageEnabled"
                                kind: "check"
                            }
                            SettingRow {
                                visible: Store.preferences.coverageEnabled
                                label: "覆盖阈值"
                                setting: "coverage"
                                kind: "slider"
                                minimum: 1
                                maximum: 100
                                suffix: "%"
                            }
                            SettingRow {
                                label: "其他应用全屏时"
                                setting: "fullscreen"
                                choices: ["保持运行", "静音", "暂停", "停止（释放内存）"]
                            }
                            SettingRow {
                                label: "其他应用播放音频时"
                                setting: "audioPlaying"
                                choices: ["保持运行", "静音", "暂停"]
                            }
                            SettingRow {
                                label: "显示器睡眠时"
                                setting: "sleep"
                                choices: ["保持运行", "暂停", "停止（释放内存）"]
                            }
                            SettingRow {
                                label: "笔记本使用电池时"
                                setting: "battery"
                                choices: ["保持运行", "暂停", "停止（释放内存）"]
                            }
                        }
                        FormSection {
                            title: "质量"
                            symbol: "sliders"
                            RowLayout {
                                Layout.fillWidth: true
                                Repeater {
                                    model: ["低", "中", "高", "极高"]
                                    MButton {
                                        required property int index
                                        required property string modelData
                                        text: Theme.t(modelData)
                                        Layout.fillWidth: true
                                        onClicked: {
                                            Store.changePreference("fps", [24, 30, 60, 120][index]);
                                            Store.changePreference("antialias", index);
                                            Store.changePreference("resolution", index < 2 ? 2 : 0);
                                        }
                                    }
                                }
                            }
                            SettingRow {
                                label: "抗锯齿"
                                setting: "antialias"
                                choices: ["关闭", "MSAA ×2", "MSAA ×4", "MSAA ×8"]
                            }
                            SettingRow {
                                label: "渲染分辨率"
                                setting: "resolution"
                                choices: ["原生（最高画质）", "75%（自动）", "50%（高性能）"]
                            }
                            SettingRow {
                                label: "启用 MetalFX（场景壁纸）"
                                setting: "metalFX"
                                kind: "check"
                                supported: false
                            }
                            SettingRow {
                                label: "壁纸加载方式"
                                setting: "loadMode"
                                choices: ["从磁盘加载（较低内存占用）", "从内存加载（减少磁盘读取）"]
                            }
                            SettingRow {
                                label: "动图预览播放方式"
                                setting: "animated"
                                choices: ["鼠标悬停时播放", "当前可见壁纸持续播放"]
                            }
                            SettingRow {
                                label: "帧率"
                                setting: "fps"
                                kind: "slider"
                                minimum: 10
                                maximum: 120
                            }
                            SettingRow {
                                label: "启用音频频谱（场景与网页壁纸）"
                                setting: "spectrum"
                                kind: "check"
                            }
                            MText {
                                text: Theme.t("渲染设置将在渲染服务连接后应用。MetalFX 为 macOS 专属功能。")
                                color: Theme.secondary
                                font.pixelSize: 11
                                Layout.fillWidth: true
                            }
                        }
                        FormSection {
                            title: "视频"
                            symbol: "play"
                            SettingRow {
                                label: "启用 HDR 视频"
                                setting: "hdr"
                                kind: "check"
                                supported: false
                            }
                        }
                    }
                    ColumnLayout {
                        visible: settings.page === 1
                        Layout.fillWidth: true
                        spacing: 20
                        FormSection {
                            title: "启动"
                            symbol: "star"
                            SettingRow {
                                label: "启动时显示"
                                setting: "startupSection"
                                choices: ["已安装", "发现", "创意工坊", "已订阅"]
                            }
                            SettingRow {
                                label: "开机时自动启动 Mirage"
                                setting: "autostart"
                                kind: "check"
                                supported: false
                            }
                            SettingRow {
                                label: "隐藏菜单栏图标"
                                setting: "hideTray"
                                kind: "check"
                                supported: false
                            }
                            SettingRow {
                                label: "菜单栏图标"
                                setting: "trayStyle"
                                choices: ["彩色", "黑白"]
                                supported: false
                            }
                        }
                        FormSection {
                            title: "软件更新"
                            symbol: "refresh"
                            SettingRow {
                                label: "自动检查并下载更新"
                                setting: "updates"
                                kind: "check"
                                supported: false
                            }
                            SettingRow {
                                label: "接收测试版更新"
                                setting: "beta"
                                kind: "check"
                                supported: false
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                MText {
                                    text: Theme.t("更新状态")
                                    Layout.fillWidth: true
                                }
                                MText {
                                    text: Theme.t("更新服务尚未连接")
                                    color: Theme.secondary
                                }
                            }
                            MButton {
                                text: Theme.t("立即检查更新")
                                enabled: false
                            }
                        }
                        FormSection {
                            title: "语言"
                            symbol: "globe"
                            RowLayout {
                                Layout.fillWidth: true
                                MText {
                                    text: Theme.t("语言")
                                    Layout.fillWidth: true
                                }
                                MCombo {
                                    model: ["跟随系统", "English", "简体中文", "繁體中文"]
                                    currentIndex: ["system", "en", "zh-Hans", "zh-Hant"].indexOf(Store.preferences.language)
                                    onActivated: Store.changePreference("language", ["system", "en", "zh-Hans", "zh-Hant"][currentIndex])
                                }
                            }
                        }
                        FormSection {
                            title: "外观"
                            symbol: "image"
                            SettingRow {
                                label: "外观"
                                setting: "appearance"
                                choices: ["浅色", "深色", "跟随系统"]
                            }
                            SettingRow {
                                label: "覆盖壁纸"
                                setting: "overrideWallpaper"
                                kind: "check"
                                supported: false
                            }
                        }
                        FormSection {
                            title: "音频"
                            symbol: "volume"
                            SettingRow {
                                label: "全局音量"
                                setting: "volume"
                                kind: "slider"
                                maximum: 1
                                step: 0.01
                                suffix: "%"
                            }
                            SettingRow {
                                label: "全局静音"
                                setting: "muted"
                                kind: "check"
                            }
                        }
                        FormSection {
                            title: "壁纸库"
                            symbol: "folder"
                            Repeater {
                                model: bridge.sources
                                ColumnLayout {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    MText {
                                        text: modelData
                                        Layout.fillWidth: true
                                        font.pixelSize: 12
                                        color: Theme.secondary
                                    }
                                    RowLayout {
                                        MButton {
                                            text: Theme.t("在文件管理器中显示")
                                            onClicked: bridge.openFolder(modelData)
                                        }
                                        MButton {
                                            text: Theme.t("移除目录")
                                            destructive: true
                                            onClicked: bridge.removeSource(modelData)
                                        }
                                    }
                                }
                            }
                            MButton {
                                text: Theme.t("选择目录…")
                                symbol: "folder"
                                onClicked: sourceFolder.open()
                            }
                            SettingRow {
                                label: "自动刷新壁纸库"
                                setting: "autoRefresh"
                                kind: "check"
                            }
                        }
                        FormSection {
                            title: "创意工坊"
                            symbol: "cloud"
                            SettingRow {
                                label: "免登录下载"
                                setting: "directDownload"
                                kind: "check"
                                supported: false
                            }
                            SettingRow {
                                label: "Steam API 线路"
                                setting: "apiEndpoint"
                                choices: ["Steam 官方 Web API", "SteamCF 镜像"]
                                supported: false
                            }
                            MButton {
                                text: Theme.t("登录 Steam")
                                symbol: "person"
                                onClicked: settings.loginRequested()
                            }
                        }
                        FormSection {
                            title: "Steam API Key"
                            symbol: "lock"
                            MText {
                                text: Theme.t("请设置您自己的 Steam Web API Key")
                                font.bold: true
                                Layout.fillWidth: true
                            }
                            MField {
                                Layout.fillWidth: true
                                placeholderText: "Steam Web API Key"
                                text: Store.preferences.apiKey
                                onTextEdited: Store.changePreference("apiKey", text)
                                maximumLength: 32
                            }
                            MText {
                                visible: Store.preferences.apiKey.length > 0 && !/^[0-9a-fA-F]{32}$/.test(Store.preferences.apiKey)
                                text: Theme.t("Steam Web API Key 格式无效")
                                color: Theme.red
                                Layout.fillWidth: true
                            }
                            MButton {
                                text: Theme.t("申请 Steam API Key")
                                flatStyle: true
                                onClicked: Qt.openUrlExternally("https://steamcommunity.com/dev/apikey")
                            }
                        }
                        FormSection {
                            title: "高级"
                            symbol: "settings"
                            SettingRow {
                                label: "开发模式"
                                setting: "developer"
                                kind: "check"
                            }
                            RowLayout {
                                MText {
                                    text: Theme.t("重置所有设置")
                                    Layout.fillWidth: true
                                }
                                MButton {
                                    text: Theme.t("重置")
                                    destructive: true
                                    prominent: true
                                    onClicked: {
                                        Store.preferences = Object.assign({}, Store.defaultPreferences);
                                        bridge.language = Store.preferences.language;
                                    }
                                }
                            }
                        }
                    }
                    ColumnLayout {
                        visible: settings.page === 2
                        Layout.fillWidth: true
                        spacing: 20
                        FormSection {
                            title: "内置插件"
                            symbol: "grid"
                            MText {
                                text: Theme.t("暂无内置插件。")
                                color: Theme.secondary
                            }
                        }
                        FormSection {
                            title: "已安装插件"
                            symbol: "folder"
                            MText {
                                text: Theme.t("无")
                                color: Theme.secondary
                            }
                        }
                    }
                    ColumnLayout {
                        visible: settings.page === 3
                        Layout.fillWidth: true
                        spacing: 20
                        FormSection {
                            title: "屏保组件"
                            symbol: "display"
                            MText {
                                text: Theme.t("屏保服务尚未连接")
                                Layout.fillWidth: true
                                color: Theme.secondary
                            }
                            RowLayout {
                                MButton {
                                    text: Theme.t("安装屏保组件")
                                    enabled: false
                                }
                                MButton {
                                    text: Theme.t("打开系统设置")
                                    enabled: false
                                }
                            }
                        }
                        FormSection {
                            title: "屏保壁纸"
                            symbol: "image"
                            MText {
                                text: Theme.t("尚未设置屏保壁纸")
                                color: Theme.secondary
                            }
                            MButton {
                                text: Theme.t("使用当前壁纸")
                                enabled: false
                            }
                            MText {
                                text: Theme.t("屏保始终静音，并保存当前预设、自定义属性、填充方式和最高 60 FPS 的帧率设置。之后对同一壁纸的自定义修改会自动同步。")
                                font.pixelSize: 12
                                color: Theme.secondary
                                Layout.fillWidth: true
                            }
                        }
                        FormSection {
                            title: "动态锁屏"
                            symbol: "lock"
                            MCheck {
                                text: Theme.t("启用动态锁屏方案 A")
                                enabled: false
                            }
                            MCheck {
                                text: Theme.t("启用动态锁屏方案 B")
                                enabled: false
                            }
                            MText {
                                text: Theme.t("此功能使用 macOS 系统扩展，当前平台不可用。")
                                font.pixelSize: 12
                                color: Theme.secondary
                                Layout.fillWidth: true
                            }
                        }
                    }
                    AboutPage {
                        visible: settings.page === 4
                        Layout.fillWidth: true
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.line
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Icon {
                name: "warning"
                color: Theme.orange
                visible: settings.dirty
            }
            MText {
                text: Theme.t("已修改")
                color: Theme.secondary
                visible: settings.dirty
            }
            Item {
                Layout.fillWidth: true
            }
            MButton {
                text: Theme.t("取消")
                implicitWidth: 70
                onClicked: settings.close()
            }
            MButton {
                text: Theme.t("好")
                implicitWidth: 70
                prominent: true
                onClicked: {
                    Store.commitPreferences();
                    settings.committed = true;
                    settings.close();
                }
            }
        }
    }
    FolderDialog {
        id: sourceFolder
        onAccepted: bridge.addSource(selectedFolder)
    }
    Shortcut {
        sequence: "Escape"
        onActivated: settings.close()
    }
}
