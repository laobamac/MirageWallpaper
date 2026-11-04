// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Sheet {
    id: sheet
    width: 540
    contentItem: ColumnLayout {
        spacing: 16
        RowLayout {
            Icon {
                name: "flame"
                color: Theme.orange
            }
            MText {
                text: Theme.t("烘焙为视频")
                font.pixelSize: 20
                font.bold: true
            }
        }
        MText {
            text: Store.selected.title || ""
            font.bold: true
        }
        FormSection {
            RowLayout {
                MText {
                    text: Theme.t("输出尺寸")
                    Layout.fillWidth: true
                }
                MCombo {
                    id: resolution
                    model: ["1280 × 720", "1920 × 1080", "2560 × 1440", "3840 × 2160", "720 × 1280", "1080 × 1920", "1440 × 2560", "2160 × 3840", "自定义"]
                    currentIndex: 1
                    onActivated: {
                        if (currentIndex < 8) {
                            let p = displayText.split(" × ");
                            widthField.text = p[0];
                            heightField.text = p[1];
                        }
                    }
                }
            }
            RowLayout {
                MField {
                    id: widthField
                    implicitWidth: 90
                    text: "1920"
                    validator: IntValidator {
                        bottom: 128
                        top: 4096
                    }
                    onTextEdited: resolution.currentIndex = 8
                }
                MText {
                    text: "×"
                }
                MField {
                    id: heightField
                    implicitWidth: 90
                    text: "1080"
                    validator: IntValidator {
                        bottom: 128
                        top: 4096
                    }
                    onTextEdited: resolution.currentIndex = 8
                }
                MText {
                    text: Theme.t("像素")
                    color: Theme.secondary
                }
            }
            RowLayout {
                MText {
                    text: Theme.t("帧率")
                    Layout.fillWidth: true
                }
                MCombo {
                    model: ["24 FPS", "30 FPS", "60 FPS"]
                    currentIndex: 1
                }
            }
            RowLayout {
                MText {
                    text: Theme.t("烘焙时长（秒）")
                    Layout.fillWidth: true
                }
                SpinBox {
                    from: 1
                    to: 600
                    value: 30
                    editable: true
                }
            }
            RowLayout {
                visible: Store.selected.type === "scene"
                MText {
                    text: Theme.t("预热")
                    Layout.fillWidth: true
                }
                SpinBox {
                    from: 0
                    to: 10
                    value: 2
                }
            }
            MCheck {
                text: Theme.t("包含壁纸自身音频")
                visible: Store.selected.type !== "web"
            }
            MCheck {
                text: Theme.t("高画质（文件更大）")
                visible: Store.selected.type !== "video"
                checked: true
            }
        }
        MText {
            text: Theme.t("烘焙会固定当前属性，生成新的 SDR 视频壁纸。鼠标交互、时钟和实时媒体信息不会继续更新；首尾不保证无缝。")
            Layout.fillWidth: true
            color: Theme.secondary
        }
        MText {
            text: Theme.t("烘焙服务尚未连接")
            Layout.fillWidth: true
            color: Theme.orange
        }
        RowLayout {
            Item {
                Layout.fillWidth: true
            }
            MButton {
                text: Theme.t("取消")
                onClicked: sheet.close()
            }
            MButton {
                text: Theme.t("开始烘焙")
                prominent: true
                enabled: false
            }
        }
    }
}
