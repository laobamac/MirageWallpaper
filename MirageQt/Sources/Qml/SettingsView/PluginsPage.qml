import QtQuick
import QtQuick.Layouts
import FluentUI

FluScrollablePage {
    required property var host

    ColumnLayout {
        Layout.fillWidth: true

                        spacing: 12
                        FluText {
                            text: "Steam 与路径"
                            font: FluTextStyle.BodyStrong
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            FluText {
                                text: "API 地址"
                            }
                            FluComboBox {
                                Layout.fillWidth: true
                                model: ["官方", "镜像"]
                                currentIndex: host.settingsDraft.steamAPIEndpoint === "mirror" ? 1 : 0
                                onActivated: host.setSetting("steamAPIEndpoint", currentIndex === 1 ? "mirror" : "official")
                            }
                        }
                        FluTextBox {
                            Layout.fillWidth: true
                            placeholderText: "Steam Web API Key（32 位十六进制）"
                            text: host.settingsDraft.steamAPIKey || ""
                            onTextChanged: host.setSetting("steamAPIKey", text)
                        }
                        FluTextBox {
                            Layout.fillWidth: true
                            placeholderText: "自定义创意工坊目录"
                            text: host.settingsDraft.customWorkshopDirectory || ""
                            onTextChanged: host.setSetting("customWorkshopDirectory", text)
                        }
                        FluTextBox {
                            Layout.fillWidth: true
                            placeholderText: "自定义导入目录"
                            text: host.settingsDraft.customImportedDirectory || ""
                            onTextChanged: host.setSetting("customImportedDirectory", text)
                        }
                        FluText {
                            Layout.fillWidth: true
                            text: "自定义目录会在下次刷新壁纸库时生效。"
                            color: FluTheme.fontSecondaryColor
                            wrapMode: Text.WordWrap
                        }
                        FluDivider {
                            Layout.fillWidth: true
                        }
                        FluText {
                            text: "外观覆盖"
                            font: FluTextStyle.BodyStrong
                        }
                        FluText {
                            Layout.fillWidth: true
                            text: "桌面壁纸覆盖（TODO：Linux 桌面覆盖服务）"
                            color: FluTheme.fontSecondaryColor
                            wrapMode: Text.WordWrap
                        }
                    }
    }
