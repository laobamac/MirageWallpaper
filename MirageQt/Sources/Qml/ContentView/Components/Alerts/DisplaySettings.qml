import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

MirageDialogWindow {
    id: root
    required property var host
    title: "选择显示器"
    width: 720
    height: 560
    neutralText: "完成"
    buttonFlags: FluContentDialogType.NeutralButton
    property string selectedDisplayId: ""

    contentDelegate: Component {
        ColumnLayout {
            width: parent.width
            spacing: 12
            FluComboBox {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                enabled: false
                model: ["每个显示器的壁纸"]
                currentIndex: 0
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                FluButton { text: "Split"; enabled: false }
                FluButton { text: "Remove Split"; enabled: false }
                FluButton { text: "Load profile"; enabled: false }
                FluButton { text: "Save profile"; enabled: false }
            }
            FluText {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                visible: mirage.displayModel.count === 0
                text: "没有可用的显示器输出"
                horizontalAlignment: Text.AlignHCenter
            }
            Item {
                id: canvas
                Layout.fillWidth: true
                Layout.preferredHeight: 280
                Layout.minimumHeight: 220
                visible: mirage.displayModel.count > 0
                Rectangle {
                    anchors.fill: parent
                    color: "#202020"
                    border.color: "#3d3d3d"
                    border.width: 1
                    radius: 3
                }
                property real scaleFactor: {
                    var geometry = mirage.displayModel.virtualGeometry
                    if (geometry.width <= 0 || geometry.height <= 0) return 1
                    return Math.min(width / geometry.width, height / geometry.height) * 0.9
                }
                Repeater {
                    model: mirage.displayModel
                    delegate: Rectangle {
                        property int displayIndex: index
                        required property string stableId
                        required property string name
                        required property int logicalX
                        required property int logicalY
                        required property int logicalWidth
                        required property int logicalHeight
                        required property bool running
                        required property string wallpaperTitle
                        required property url wallpaperPreview
                        readonly property var virtualGeometry: mirage.displayModel.virtualGeometry
                        x: (logicalX - virtualGeometry.x) * canvas.scaleFactor + (canvas.width - virtualGeometry.width * canvas.scaleFactor) / 2
                        y: (logicalY - virtualGeometry.y) * canvas.scaleFactor + (canvas.height - virtualGeometry.height * canvas.scaleFactor) / 2
                        width: Math.max(100, logicalWidth * canvas.scaleFactor)
                        height: Math.max(70, logicalHeight * canvas.scaleFactor)
                        color: stableId === root.selectedDisplayId ? "#2563eb" : (running ? "#1f2937" : "#374151")
                        border.width: stableId === root.selectedDisplayId ? 3 : 1
                        border.color: stableId === root.selectedDisplayId ? "#93c5fd" : "#718096"
                        radius: 4
                        Image {
                            anchors.fill: parent
                            anchors.margins: 2
                            source: wallpaperPreview
                            visible: source !== ""
                            fillMode: Image.PreserveAspectCrop
                            opacity: 0.45
                            z: 0
                        }
                        Column {
                            z: 1
                            anchors.centerIn: parent
                            width: parent.width - 12
                            spacing: 3
                            FluText { width: parent.width; text: name; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight; color: "white" }
                            FluText { width: parent.width; text: "S" + (displayIndex + 1); horizontalAlignment: Text.AlignHCenter; color: "#bfdbfe" }
                            FluText { width: parent.width; text: logicalWidth + " × " + logicalHeight; horizontalAlignment: Text.AlignHCenter; color: "white" }
                            FluText { width: parent.width; text: running ? wallpaperTitle : "未渲染壁纸"; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight; color: "#d1d5db" }
                        }
                        MouseArea { anchors.fill: parent; onClicked: root.selectedDisplayId = stableId }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 42
                FluFilledButton {
                    text: "更换壁纸"
                    enabled: root.selectedDisplayId.length > 0 && mirage.selectedWallpaperId.length > 0
                    onClicked: {
                        mirage.beginWallpaperAssignment(root.selectedDisplayId)
                        root.close()
                        root.host.currentTab = 0
                    }
                }
                FluButton {
                    text: "移除壁纸"
                    enabled: root.selectedDisplayId.length > 0
                    onClicked: mirage.removeWallpaperFromDisplay(root.selectedDisplayId)
                }
                Item { Layout.fillWidth: true }
                FluCheckBox {
                    text: "启动时显示"
                    checked: mirage.showOnStart
                    onClicked: mirage.showOnStart = checked
                }
            }
        }
    }

    onOpened: {
        if (mirage.displayModel.count > 0 && selectedDisplayId.length === 0)
            selectedDisplayId = mirage.displayModel.stableIdAt(0)
    }
}
