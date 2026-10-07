import QtQuick
import FluentUI

// 三个方形网格共用的图标尺寸菜单。分页协议固定为每页 50 项，
// 因此菜单只修改卡片最小宽度，不得通过视图偏好改变服务端或本地分页容量。
FluDropDownButton {
    id: menu

    property int explorerIconSize: 170
    signal iconSizeChanged(int size)

    text: "视图"

    FluMenuItem {
        text: qsTr("小图标")
        checkable: true
        checked: menu.explorerIconSize === 140
        onTriggered: menu.iconSizeChanged(140)
    }
    FluMenuItem {
        text: qsTr("中图标")
        checkable: true
        checked: menu.explorerIconSize === 170
        onTriggered: menu.iconSizeChanged(170)
    }
    FluMenuItem {
        text: qsTr("大图标")
        checkable: true
        checked: menu.explorerIconSize === 200
        onTriggered: menu.iconSizeChanged(200)
    }
}
