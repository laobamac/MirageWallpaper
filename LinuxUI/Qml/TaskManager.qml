// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: manager
    property bool baking: false
    readonly property var tasks: baking ? Store.bakeTasks : Store.downloads
    spacing: 10
    RowLayout {
        Layout.fillWidth: true
        MText {
            text: Theme.t(manager.baking ? "烘焙任务" : "下载管理")
            font.pixelSize: 18
            font.bold: true
            Layout.fillWidth: true
        }
        MButton {
            text: Theme.t("清除记录")
            flatStyle: true
            enabled: manager.tasks.some(x => ["complete", "completed", "failed"].includes(x.state))
            onClicked: {
                if (manager.baking)
                    Store.bakeTasks = Store.bakeTasks.filter(x => !["complete", "failed"].includes(x.state));
                else
                    Store.downloads = Store.downloads.filter(x => !["completed", "failed"].includes(x.state));
            }
        }
    }
    Rectangle {
        height: 1
        Layout.fillWidth: true
        color: Theme.line
    }
    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: 300
        EmptyState {
            anchors.fill: parent
            visible: manager.tasks.length === 0
            symbol: manager.baking ? "flame" : "download"
            title: Theme.t(manager.baking ? "暂无烘焙任务" : "暂无下载任务")
            subtitle: Theme.t(manager.baking ? "烘焙服务尚未连接" : "在创意工坊中浏览并下载壁纸")
        }
        ListView {
            anchors.fill: parent
            clip: true
            model: manager.tasks
            spacing: 1
            delegate: Rectangle {
                required property var modelData
                width: ListView.view.width
                implicitHeight: 100
                color: Theme.field
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10
                    Image {
                        source: modelData.preview || ""
                        Layout.preferredWidth: 64
                        Layout.preferredHeight: 64
                        fillMode: Image.PreserveAspectCrop
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        MText {
                            text: modelData.title
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            font.bold: manager.baking
                        }
                        MText {
                            text: modelData.error || Theme.t(({
                                    queued: "等待可用下载槽…",
                                    resolving: "正在解析创意工坊内容…",
                                    downloading: "下载中",
                                    validating: "验证中...",
                                    completed: "已完成",
                                    complete: "已完成",
                                    failed: "失败",
                                    rendering: "正在烘焙"
                                })[modelData.state] || modelData.state)
                            color: modelData.state === "failed" ? Theme.red : Theme.secondary
                            font.pixelSize: 11
                            Layout.fillWidth: true
                        }
                        ProgressBar {
                            visible: ["downloading", "rendering", "complete", "completed"].includes(modelData.state)
                            Layout.fillWidth: true
                            value: modelData.progress || 0
                        }
                        MText {
                            visible: !!modelData.detail
                            text: modelData.detail || ""
                            font.pixelSize: 11
                            color: Theme.secondary
                        }
                    }
                    MButton {
                        symbol: modelData.state === "failed" ? "refresh" : ["complete", "completed"].includes(modelData.state) ? "folder" : "close"
                        flatStyle: true
                        onClicked: {
                            if (modelData.path && ["complete", "completed"].includes(modelData.state))
                                bridge.openFolder(modelData.path);
                            else
                                Store.request(modelData.state === "failed" ? "retryTask" : "cancelTask", {
                                    id: modelData.id,
                                    baking: manager.baking
                                });
                        }
                    }
                }
            }
            ScrollBar.vertical: ScrollBar {}
        }
    }
    Rectangle {
        height: 1
        Layout.fillWidth: true
        color: Theme.line
    }
    RowLayout {
        MText {
            text: manager.tasks.filter(x => !["completed", "complete", "failed"].includes(x.state)).length + " · " + Theme.t("进行中")
            color: Theme.secondary
            font.pixelSize: 11
        }
        Item {
            Layout.fillWidth: true
        }
        MText {
            text: manager.tasks.filter(x => ["completed", "complete"].includes(x.state)).length + " · " + Theme.t("已完成")
            color: "#34c759"
            font.pixelSize: 11
        }
    }
}
