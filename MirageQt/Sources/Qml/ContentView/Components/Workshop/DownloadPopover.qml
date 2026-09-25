import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import FluentUI
import "../../../GlobalComponents"

FluPopup {
    id: popup

    property Item anchorItem
    // WorkshopView always supplies MirageController.downloadQueue().  Its
    // records have one fixed task protocol, so no alternate task shape is
    // accepted here.
    required property var tasks
    property var queue: tasks

    width: 430
    height: Math.min(520, Math.max(180, queue.length > 0 ? 490 : 230))
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function openFor(item) {
        anchorItem = item;
        open();
        Qt.callLater(positionNearAnchor);
    }

    function positionNearAnchor() {
        if (!anchorItem || !parent)
            return;
        var point = anchorItem.mapToItem(parent, 0, anchorItem.height);
        x = Math.max(8, Math.min(parent.width - width - 8, point.x + anchorItem.width - width));
        y = Math.max(8, Math.min(parent.height - height - 8, point.y + 4));
    }

    onOpened: positionNearAnchor()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 0
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            Layout.leftMargin: 16
            Layout.rightMargin: 10
            FluText {
                text: qsTr("下载管理")
                font: FluTextStyle.Subtitle
            }
            Item { Layout.fillWidth: true }
            FluButton {
                visible: popup.queue.some(function(task) {
                    return task.state === "completed" || task.state === "failed"
                        || task.state === "cancelled";
                })
                text: qsTr("清除记录")
                onClicked: mirage.clearCompletedDownloads()
            }
        }

        FluDivider { Layout.fillWidth: true }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignHCenter
            visible: popup.queue.length === 0
            spacing: 8
            FluIcon {
                Layout.alignment: Qt.AlignHCenter
                iconSource: FluentIcons.Download
                iconSize: 34
                iconColor: FluTheme.fontTertiaryColor
            }
            FluText {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("暂无下载任务")
                color: FluTheme.fontSecondaryColor
            }
            FluText {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("在创意工坊中浏览并下载壁纸")
                color: FluTheme.fontTertiaryColor
                font: FluTextStyle.Caption
            }
        }

        ListView {
            id: taskList
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: popup.queue.length > 0
            clip: true
            model: popup.queue
            delegate: FluFrame {
                required property var modelData
                width: taskList.width
                implicitHeight: 86
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 9
                    spacing: 5
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        WorkshopImage {
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 58
                            imageUrl: modelData.preview
                            contentMode: Image.PreserveAspectCrop
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            RowLayout {
                                Layout.fillWidth: true
                                FluText {
                                    Layout.fillWidth: true
                                    text: modelData.title
                                    elide: Text.ElideRight
                                    font: FluTextStyle.BodyStrong
                                }
                                FluText {
                                    visible: modelData.purpose === "presetDependency"
                                    text: qsTr("基础壁纸")
                                    color: Qt.rgba(196 / 255, 121 / 255, 0, 1)
                                    font: FluTextStyle.Caption
                                }
                            }
                            FluProgressBar {
                                Layout.fillWidth: true
                                visible: ["starting", "connecting", "downloading", "resolving"]
                                    .indexOf(modelData.state) >= 0
                                indeterminate: modelData.state === "starting"
                                    || modelData.state === "connecting" || modelData.state === "resolving"
                                from: 0
                                to: 1
                                value: modelData.progress / 100
                            }
                            FluText {
                                Layout.fillWidth: true
                                text: {
                                    var state = modelData.state;
                                    if (modelData.message.length > 0)
                                        return modelData.message;
                                    if (state === "queued") return qsTr("等待 Steam 服务按顺序下载…");
                                    if (state === "starting" || state === "connecting")
                                        return qsTr("正在连接 Steam 服务…");
                                    if (state === "downloading")
                                        return qsTr("正在下载 (%1%)").arg(Math.round(modelData.progress));
                                    if (state === "resolving") return qsTr("正在处理下载...");
                                    if (state === "completed") return qsTr("已完成");
                                    if (state === "cancelled") return qsTr("已取消");
                                    if (state === "failed") return qsTr("失败");
                                }
                                elide: Text.ElideRight
                                color: modelData.state === "failed"
                                    ? Qt.rgba(196 / 255, 43 / 255, 28 / 255, 1)
                                    : FluTheme.fontSecondaryColor
                                font: FluTextStyle.Caption
                            }
                        }
                        FluIconButton {
                            iconSource: {
                                var state = modelData.state;
                                if (state === "failed") return FluentIcons.Refresh;
                                if (state === "completed") return FluentIcons.FolderOpen;
                                return FluentIcons.Cancel;
                            }
                            text: {
                                var state = modelData.state;
                                if (state === "failed") return qsTr("重试");
                                if (state === "completed") return qsTr("打开下载目录");
                                return qsTr("取消下载");
                            }
                            contentDescription: text
                            onClicked: {
                                var state = modelData.state;
                                var id = modelData.id;
                                if (state === "failed") {
                                    mirage.retryWorkshopDownload(id);
                                } else if (state === "completed") {
                                    mirage.revealWorkshopDownload(id);
                                } else {
                                    mirage.cancelWorkshopDownload(id);
                                }
                            }
                        }
                    }
                }
            }
        }

        FluDivider { Layout.fillWidth: true }
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            FluText {
                text: qsTr("%1 下载中").arg(mirage.activeDownloadCount)
                color: FluTheme.fontSecondaryColor
                font: FluTextStyle.Caption
            }
            Item { Layout.fillWidth: true }
            FluText {
                text: qsTr("%1 已完成").arg(popup.queue.filter(function(task) {
                    return task.state === "completed";
                }).length)
                color: Qt.rgba(16 / 255, 124 / 255, 16 / 255, 1)
                font: FluTextStyle.Caption
            }
        }
    }
}
