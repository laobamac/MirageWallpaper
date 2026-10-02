// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ColumnLayout {
    id: editor
    property var rows: Store.selected.properties || []
    property var overrides: Store.selectedRuntime.properties || ({})
    onRowsChanged: conditionTimer.restart()
    onOverridesChanged: conditionTimer.restart()
    Timer {
        id: conditionTimer
        interval: 16
        onTriggered: bridge.evaluateConditions(editor.rows, editor.overrides)
    }
    spacing: 12
    Repeater {
        model: editor.rows
        ColumnLayout {
            id: entry
            required property var modelData
            property var stored: Store.selectedRuntime.properties || ({})
            property var value: stored[modelData.key] === undefined ? modelData.value : stored[modelData.key]
            property var visibleOptions: (modelData.options || []).filter(option => !option.condition || bridge.conditions[option.condition] !== false)
            visible: (!modelData.condition || bridge.conditions[modelData.condition] !== false) && ["bool", "slider", "color", "combo", "textinput", "text", "group", "file", "scenetexture", "directory", "usershortcut"].includes(modelData.type)
            Layout.fillWidth: true
            spacing: 6
            MText {
                Layout.fillWidth: true
                text: Theme.p(entry.modelData.text || entry.modelData.key)
                textFormat: Text.RichText
                onLinkActivated: link => Qt.openUrlExternally(link)
                font.weight: entry.modelData.type === "group" ? Font.DemiBold : Font.Normal
                visible: !["bool", "slider", "color", "combo"].includes(entry.modelData.type)
            }
            MCheck {
                visible: entry.modelData.type === "bool"
                text: Theme.p(entry.modelData.text || entry.modelData.key)
                checked: Boolean(entry.value)
                Layout.fillWidth: true
                onClicked: Store.setProperty(entry.modelData.key, checked)
            }
            ColumnLayout {
                visible: entry.modelData.type === "slider"
                Layout.fillWidth: true
                spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    MText {
                        text: Theme.p(entry.modelData.text || entry.modelData.key)
                        Layout.fillWidth: true
                        maximumLineCount: 1
                        elide: Text.ElideRight
                    }
                    MText {
                        text: entry.modelData.fraction ? Number(entry.value || 0).toFixed(2) : String(Math.round(Number(entry.value || 0)))
                        font.pixelSize: 11
                        color: Theme.secondary
                    }
                }
                MSlider {
                    Layout.fillWidth: true
                    from: Number(entry.modelData.min === undefined ? 0 : entry.modelData.min)
                    to: Number(entry.modelData.max === undefined ? 1 : entry.modelData.max)
                    stepSize: Number(entry.modelData.step || (entry.modelData.fraction ? 0.01 : 1))
                    value: Number(entry.value || 0)
                    onMoved: Store.setProperty(entry.modelData.key, value)
                }
            }
            RowLayout {
                visible: entry.modelData.type === "combo"
                Layout.fillWidth: true
                MText {
                    text: Theme.p(entry.modelData.text || entry.modelData.key)
                    Layout.fillWidth: true
                }
                MCombo {
                    Layout.maximumWidth: 170
                    model: entry.visibleOptions.map(o => Theme.p(o.label))
                    translateModel: false
                    currentIndex: entry.visibleOptions.findIndex(o => o.value === entry.value)
                    onActivated: Store.setProperty(entry.modelData.key, entry.visibleOptions[currentIndex].value)
                }
            }
            RowLayout {
                visible: entry.modelData.type === "color"
                Layout.fillWidth: true
                MText {
                    text: Theme.p(entry.modelData.text || entry.modelData.key)
                    Layout.fillWidth: true
                }
                Rectangle {
                    width: 36
                    height: 22
                    radius: 4
                    border.color: Theme.line
                    color: {
                        let c = String(entry.value || "1 1 1").split(" ");
                        return Qt.rgba(Number(c[0]), Number(c[1]), Number(c[2]), 1);
                    }
                    TapHandler {
                        onTapped: {
                            colorPicker.selectedColor = parent.color;
                            colorPicker.open();
                        }
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: Theme.p(entry.modelData.text || entry.modelData.key)
                }
                ColorDialog {
                    id: colorPicker
                    title: Theme.p(entry.modelData.text || entry.modelData.key)
                    onAccepted: Store.setProperty(entry.modelData.key, [selectedColor.r, selectedColor.g, selectedColor.b].join(" "))
                }
            }
            MField {
                visible: entry.modelData.type === "textinput"
                Layout.fillWidth: true
                text: String(entry.value || "")
                onEditingFinished: Store.setProperty(entry.modelData.key, text)
            }
            RowLayout {
                visible: ["file", "directory", "scenetexture", "usershortcut"].includes(entry.modelData.type)
                Layout.fillWidth: true
                MField {
                    Layout.fillWidth: true
                    text: String(entry.value || "")
                    readOnly: entry.modelData.type !== "usershortcut"
                    onEditingFinished: if (entry.modelData.type === "usershortcut")
                        Store.setProperty(entry.modelData.key, text)
                }
                MButton {
                    symbol: "folder"
                    onClicked: {
                        if (entry.modelData.type === "directory")
                            propertyFolder.open();
                        else
                            propertyFile.open();
                    }
                }
                FolderDialog {
                    id: propertyFolder
                    onAccepted: Store.setProperty(entry.modelData.key, selectedFolder.toString())
                }
                FileDialog {
                    id: propertyFile
                    title: Theme.t("选择文件")
                    onAccepted: Store.setProperty(entry.modelData.key, selectedFile.toString())
                }
            }
            Rectangle {
                visible: entry.modelData.type === "group"
                height: 1
                Layout.fillWidth: true
                color: Theme.line
            }
        }
    }
    MText {
        visible: editor.rows.length === 0
        text: Theme.t("此壁纸没有可自定义的属性")
        color: Theme.secondary
        font.pixelSize: 12
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignHCenter
        Layout.topMargin: 12
        Layout.bottomMargin: 12
    }
}
