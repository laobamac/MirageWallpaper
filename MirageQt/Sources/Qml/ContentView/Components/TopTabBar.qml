import QtQuick
import QtQuick.Layouts
import FluentUI
import "../../GlobalComponents"

RowLayout {
    id: bar

    property int currentIndex: 0
    signal selected(int index)
    signal mobileRequested
    signal displayRequested
    signal settingsRequested

    onCurrentIndexChanged: pivot.currentIndex = currentIndex

    // 把内部可交互控件注册为 frameless 顶栏的可点击区域，
    // 这样标签和图标按钮能收到点击，而顶栏空白区仍可拖动窗口。
    function registerHitTest(host) {
        var stack = [bar];
        while (stack.length > 0) {
            var item = stack.pop();
            if (item instanceof FluPivot || item instanceof FluIconButton)
                host.setHitTestVisible(item);
            for (var i = 0; i < item.children.length; ++i)
                stack.push(item.children[i]);
        }
    }

    FluPivot {
        id: pivot
        Layout.preferredWidth: 240
        Layout.preferredHeight: 30
        Component.onCompleted: currentIndex = bar.currentIndex
        onCurrentIndexChanged: bar.selected(pivot.currentIndex)
        headerHeight: 30
        font: FluTextStyle.Caption
        FluPivotItem {
            title: qsTr("已安装")
        }
        FluPivotItem {
            title: qsTr("发现")
        }
        FluPivotItem {
            title: qsTr("创意工坊")
        }
        // 已订阅为独立分区（对齐 macOS MainSection.subscriptions）。
        FluPivotItem {
            title: qsTr("已订阅")
        }
    }

    Item { Layout.fillWidth: true }
    FluIconButton {
        iconSource: FluentIcons.MobileTablet
        iconSize: 15
        text: qsTr("移动端")
        contentDescription: qsTr("移动端")
        onClicked: bar.mobileRequested()
    }
    FluIconButton {
        iconSource: FluentIcons.SettingsDisplaySound
        iconSize: 15
        text: qsTr("显示器设置")
        contentDescription: qsTr("显示器设置")
        onClicked: bar.displayRequested()
    }
    FluIconButton {
        iconSource: FluentIcons.Settings
        iconSize: 15
        text: qsTr("设置")
        contentDescription: qsTr("设置")
        onClicked: bar.settingsRequested()
    }
}
