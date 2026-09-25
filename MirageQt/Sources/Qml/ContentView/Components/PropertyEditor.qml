import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../GlobalComponents"

ColumnLayout {
    id: root
    required property var host
    property var properties: []

    Repeater {
        model: root.properties
        delegate: ColumnLayout {
            id: delegateItem
            required property var modelData
            property var propertyData: modelData
            Layout.fillWidth: true
            visible: root.host.propertyConditionVisible(propertyData.condition)
            spacing: 6

            // 每个属性只实例化与类型匹配的一个分支（Loader 惰性实例化）。
            // 原实现把 9 个类型分支全部作为子项实例化，visible:false 只隐藏
            // 不省创建——属性多的壁纸在选中切换时 Repeater 销毁重建上千个
            // QML 对象导致卡顿；与 macOS PropertyRow 的 switch(propertyType)
            // 只构建匹配分支对齐。分支组件经 Loader 的动态作用域访问 delegate
            // 的 propertyData / root.host / mirage（QML Loader 官方模式）。
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "bool"
                sourceComponent: boolEditor
            }
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "slider"
                sourceComponent: sliderEditor
            }
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "color"
                sourceComponent: colorEditor
            }
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "combo"
                sourceComponent: comboEditor
            }
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "textinput"
                sourceComponent: textInputEditor
            }
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "text"
                sourceComponent: textEditor
            }
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "group"
                sourceComponent: groupEditor
            }
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "file" || propertyData.type === "directory" || propertyData.type === "scenetexture"
                sourceComponent: fileEditor
            }
            Loader {
                Layout.fillWidth: true
                active: propertyData.type === "usershortcut"
                sourceComponent: userShortcutEditor
            }

            Component {
                id: boolEditor
                RowLayout {
                    width: parent.width
                    spacing: 8
                    FluText {
                        Layout.fillWidth: true
                        text: root.host.propertyLabel(propertyData)
                        wrapMode: Text.WordWrap
                    }
                    FluToggleSwitch {
                        text: ""
                        checked: root.host.propertyBoolValue(propertyData.value)
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                        clickListener: function () {
                            mirage.setSelectedProperty(propertyData.key, !checked);
                        }
                    }
                }
            }

            Component {
                id: sliderEditor
                ColumnLayout {
                    width: parent.width
                    spacing: 3
                    property real lowerBound: propertyData.hasMin ? Number(propertyData.min) : 0
                    property real upperBound: propertyData.hasMax ? Number(propertyData.max) : lowerBound + 1
                    RowLayout {
                        Layout.fillWidth: true
                        FluText {
                            Layout.fillWidth: true
                            text: root.host.propertyLabel(propertyData)
                            wrapMode: Text.WordWrap
                        }
                        FluText {
                            Layout.alignment: Qt.AlignRight | Qt.AlignTop
                            text: propertyData.fraction ? Number(propertyData.value).toFixed(2) : String(Math.round(Number(propertyData.value)))
                        }
                    }
                    FluSlider {
                        Layout.fillWidth: true
                        from: parent.lowerBound
                        to: Math.max(parent.upperBound, parent.lowerBound + 1)
                        stepSize: propertyData.hasStep ? Number(propertyData.step) : (propertyData.fraction ? 0.01 : 1)
                        value: Math.max(from, Math.min(to, Number(propertyData.value)))
                        onMoved: mirage.setSelectedProperty(propertyData.key, propertyData.fraction ? value : Math.round(value))
                    }
                }
            }

            Component {
                id: colorEditor
                ColumnLayout {
                    width: parent.width
                    spacing: 4
                    FluText {
                        Layout.fillWidth: true
                        text: root.host.propertyLabel(propertyData)
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item {
                            Layout.fillWidth: true
                        }
                        FluColorPicker {
                            current: root.host.propertyColor(propertyData.value)
                            onAccepted: mirage.setSelectedProperty(propertyData.key, root.host.propertyColorValue(current))
                        }
                    }
                }
            }

            Component {
                id: comboEditor
                ColumnLayout {
                    id: comboRow
                    width: parent.width
                    spacing: 4
                    property var optionItems: root.host.propertyOptionItems(propertyData.options)
                    FluText {
                        Layout.fillWidth: true
                        text: root.host.propertyLabel(propertyData)
                        wrapMode: Text.WordWrap
                    }
                    FluComboBox {
                        Layout.fillWidth: true
                        model: comboRow.optionItems
                        textRole: "label"
                        currentIndex: root.host.propertyOptionIndex(comboRow.optionItems, propertyData.value)
                        onActivated: {
                            if (currentIndex >= 0 && currentIndex < comboRow.optionItems.length) {
                                mirage.setSelectedProperty(propertyData.key, comboRow.optionItems[currentIndex].value);
                            }
                        }
                    }
                }
            }

            Component {
                id: textInputEditor
                ColumnLayout {
                    width: parent.width
                    spacing: 3
                    FluText {
                        Layout.fillWidth: true
                        text: root.host.propertyLabel(propertyData)
                        wrapMode: Text.WordWrap
                    }
                    FluTextBox {
                        Layout.fillWidth: true
                        text: String(propertyData.value || "")
                        onCommit: mirage.setSelectedProperty(propertyData.key, text)
                    }
                }
            }

            Component {
                id: textEditor
                // text 类型属性：内容为 WE HTML 标签文本（可能含富标签），
                // 用 RichHTMLText 渲染（对齐 macOS PropertyEditor 的
                // RichHTMLText 路径），富标签显示格式化内容、其余为纯文本。
                RichHTMLText {
                    width: parent.width
                    html: String(propertyData.text || propertyData.key || "")
                }
            }

            Component {
                id: groupEditor
                ColumnLayout {
                    width: parent.width
                    spacing: 3
                    FluText {
                        Layout.fillWidth: true
                        text: root.host.propertyLabel(propertyData)
                        wrapMode: Text.WordWrap
                        font: FluTextStyle.BodyStrong
                    }
                    FluDivider {
                        Layout.fillWidth: true
                    }
                }
            }

            Component {
                id: fileEditor
                ColumnLayout {
                    width: parent.width
                    spacing: 3
                    FluText {
                        Layout.fillWidth: true
                        text: root.host.propertyLabel(propertyData)
                        wrapMode: Text.WordWrap
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        FluText {
                            Layout.fillWidth: true
                            text: root.host.propertyPathLabel(propertyData.value)
                            elide: Text.ElideMiddle
                        }
                        FluIconButton {
                            text: "清除文件选择"
                            iconSource: FluentIcons.Clear
                            visible: String(propertyData.value || "").length > 0
                            onClicked: mirage.setSelectedProperty(propertyData.key, "")
                        }
                        FluButton {
                            text: "选择"
                            onClicked: {
                                root.host.openPropertyPicker(propertyData.key,
                                                             propertyData.type === "directory");
                            }
                        }
                    }
                }
            }

            Component {
                id: userShortcutEditor
                ColumnLayout {
                    width: parent.width
                    spacing: 4
                    FluText {
                        Layout.fillWidth: true
                        text: root.host.propertyLabel(propertyData)
                        wrapMode: Text.WordWrap
                    }
                    FluTextBox {
                        Layout.fillWidth: true
                        placeholderText: "快捷方式"
                        text: String(propertyData.value || "")
                        onCommit: mirage.setSelectedProperty(propertyData.key, text)
                    }
                }
            }
        }
    }
}
