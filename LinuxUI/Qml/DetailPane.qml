// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Rectangle {
    id: detail
    color: Theme.panel
    signal closeRequested
    signal metadataRequested
    signal contextRequested
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ColumnLayout {
                width: scroll.availableWidth
                spacing: 16
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: 16
                    spacing: 12
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(width, 288)
                        radius: 6
                        color: Theme.hover
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: Store.selected.preview || ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Icon {
                            anchors.centerIn: parent
                            width: 56
                            height: 56
                            name: "image"
                            color: Theme.secondary
                            visible: !Store.selected.preview
                            opacity: 0.45
                        }
                        MButton {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 8
                            symbol: "play"
                            prominent: true
                            enabled: !!Store.selected.id && Store.rendererConnected
                            hint: Theme.t("渲染服务尚未连接")
                            onClicked: Store.apply()
                        }
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        MText {
                            text: Store.selected.title || Theme.t("请选择一个有效的壁纸")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                        }
                        MButton {
                            symbol: "edit"
                            flatStyle: true
                            enabled: !!Store.selected.id
                            onClicked: detail.metadataRequested()
                        }
                    }
                    MButton {
                        text: Store.selected.author || Theme.t("佚名作者")
                        symbol: "person"
                        flatStyle: true
                        Layout.alignment: Qt.AlignHCenter
                        enabled: !!Store.selected.author
                        onClicked: detail.contextRequested()
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 4
                        Repeater {
                            model: 5
                            Icon {
                                name: "star"
                                width: 12
                                height: 12
                                color: Theme.secondary
                            }
                        }
                        MButton {
                            symbol: "heart"
                            flatStyle: true
                            destructive: Store.favorites.includes(Store.selectedId)
                            enabled: !!Store.selected.id
                            onClicked: Store.favorite(Store.selectedId)
                        }
                    }
                    MText {
                        text: Theme.t(({
                                scene: "场景",
                                video: "视频",
                                web: "网页",
                                preset: "预设"
                            })[Store.selected.type] || "")
                        color: Theme.secondary
                        font.pixelSize: 11
                        Layout.alignment: Qt.AlignHCenter
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 5
                        Repeater {
                            model: Store.selected.tags || []
                            Rectangle {
                                required property string modelData
                                width: tag.implicitWidth + 12
                                height: 22
                                radius: 5
                                color: Theme.hover
                                MText {
                                    id: tag
                                    anchors.centerIn: parent
                                    text: Theme.p(modelData)
                                    font.pixelSize: 11
                                }
                            }
                        }
                    }
                    SectionTitle {
                        text: Theme.t("播放控制")
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        MText {
                            text: Theme.t("音量")
                            Layout.fillWidth: true
                        }
                        MSlider {
                            implicitWidth: 100
                            from: 0
                            to: 1
                            value: Store.selectedRuntime.volume
                            enabled: !!Store.selected.id
                            onMoved: Store.setRuntime("volume", value)
                        }
                        MText {
                            text: Math.round(Store.selectedRuntime.volume * 100) + "%"
                            Layout.preferredWidth: 35
                        }
                    }
                    RowLayout {
                        visible: Store.selected.type !== "web"
                        Layout.fillWidth: true
                        MText {
                            text: Theme.t("速度")
                            Layout.fillWidth: true
                        }
                        MSlider {
                            implicitWidth: 100
                            from: 0
                            to: 2
                            stepSize: 0.1
                            value: Store.selectedRuntime.speed
                            enabled: !!Store.selected.id
                            onMoved: Store.setRuntime("speed", value)
                        }
                        MText {
                            text: Number(Store.selectedRuntime.speed).toFixed(1) + "x"
                            Layout.preferredWidth: 35
                        }
                    }
                    RowLayout {
                        visible: Store.selected.type === "video"
                        Layout.fillWidth: true
                        MText {
                            text: Theme.t("填充模式")
                            Layout.fillWidth: true
                        }
                        MCombo {
                            implicitWidth: 120
                            model: ["填充", "适应", "拉伸"]
                            currentIndex: Store.selectedRuntime.fill
                            onActivated: Store.setRuntime("fill", currentIndex)
                        }
                    }
                    SectionTitle {
                        text: Theme.t("画面位置")
                        visible: Store.selected.type !== "web"
                        Layout.fillWidth: true
                    }
                    Repeater {
                        model: Store.selected.type !== "web" ? ["x", "y"] : []
                        RowLayout {
                            required property string modelData
                            Layout.fillWidth: true
                            MText {
                                text: Theme.t(modelData === "x" ? "水平位置（X）" : "垂直位置（Y）")
                                Layout.fillWidth: true
                                font.pixelSize: 12
                            }
                            MSlider {
                                implicitWidth: 85
                                from: 0
                                to: 100
                                value: Store.selectedRuntime[parent.modelData]
                                enabled: !!Store.selected.id && Store.selectedRuntime.fill === 0
                                onMoved: Store.setRuntime(parent.modelData, value)
                            }
                            MText {
                                text: Math.round(Store.selectedRuntime[parent.modelData]) + "%"
                                Layout.preferredWidth: 35
                                font.pixelSize: 12
                            }
                        }
                    }
                    SectionTitle {
                        text: Theme.t("壁纸属性")
                        Layout.fillWidth: true
                    }
                    PropertyEditor {
                        Layout.fillWidth: true
                        enabled: !!Store.selected.id
                    }
                    SectionTitle {
                        text: Theme.t("壁纸")
                        Layout.fillWidth: true
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        MButton {
                            text: Theme.t("覆盖到所有显示器")
                            symbol: "display"
                            prominent: true
                            Layout.fillWidth: true
                            enabled: Store.rendererConnected && !!Store.selected.id
                            hint: Theme.t("渲染服务尚未连接")
                            onClicked: Store.request("applyAll", {
                                id: Store.selectedId
                            })
                        }
                        MButton {
                            text: Theme.t("停止此显示器")
                            symbol: "stop"
                            Layout.fillWidth: true
                            enabled: Store.rendererConnected
                            onClicked: Store.request("stop", {
                                display: Store.display
                            })
                        }
                        MButton {
                            text: Theme.t("全部停止")
                            symbol: "stop"
                            Layout.fillWidth: true
                            enabled: Store.rendererConnected
                            onClicked: Store.request("stopAll", {})
                        }
                    }
                    SectionTitle {
                        text: Theme.t("预设")
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        MButton {
                            text: Theme.t("导入")
                            symbol: "folder"
                            Layout.fillWidth: true
                            enabled: !!Store.selected.id
                            onClicked: presetImport.open()
                        }
                        MButton {
                            text: Theme.t("导出")
                            symbol: "download"
                            Layout.fillWidth: true
                            enabled: !!Store.selected.id
                            onClicked: presetExport.open()
                        }
                    }
                    MButton {
                        text: Theme.t("重置为默认")
                        symbol: "refresh"
                        prominent: true
                        destructive: true
                        Layout.fillWidth: true
                        enabled: !!Store.selected.id
                        onClicked: resetSheet.open()
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            Item {
                Layout.fillWidth: true
            }
            MButton {
                text: Theme.t("确定")
                implicitWidth: 70
                prominent: true
                onClicked: detail.closeRequested()
            }
            MButton {
                text: Theme.t("取消")
                implicitWidth: 70
                onClicked: detail.closeRequested()
            }
        }
    }
    FileDialog {
        id: presetImport
        nameFilters: ["JSON (*.json)"]
        onAccepted: {
            const data = bridge.importPreset(selectedFile);
            for (const key in data)
                Store.setRuntime(key, data[key]);
        }
    }
    FileDialog {
        id: presetExport
        fileMode: FileDialog.SaveFile
        nameFilters: ["JSON (*.json)"]
        defaultSuffix: "json"
        onAccepted: bridge.exportPreset(selectedFile, Store.selectedRuntime)
    }
    Sheet {
        id: resetSheet
        contentItem: ColumnLayout {
            spacing: 20
            MText {
                text: Theme.t("重置为默认")
                font.pixelSize: 18
                font.bold: true
            }
            MText {
                text: Theme.t("确定要重置此壁纸的所有自定义属性吗？")
                Layout.fillWidth: true
            }
            RowLayout {
                Item {
                    Layout.fillWidth: true
                }
                MButton {
                    text: Theme.t("取消")
                    onClicked: resetSheet.close()
                }
                MButton {
                    text: Theme.t("重置")
                    prominent: true
                    destructive: true
                    onClicked: {
                        Store.setRuntime("properties", {});
                        resetSheet.close();
                    }
                }
            }
        }
    }
}
