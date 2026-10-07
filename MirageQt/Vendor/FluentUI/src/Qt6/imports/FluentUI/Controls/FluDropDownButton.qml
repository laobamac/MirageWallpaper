import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import QtQuick.Window
import FluentUI

FluButton {
    id: control
    default property alias contentData: menu.contentData
    rightPadding:35
    verticalPadding: 0
    horizontalPadding:12
    FluIcon{
        iconSource:FluentIcons.ChevronDown
        iconSize: 15
        anchors{
            right: parent.right
            rightMargin: 10
            verticalCenter: parent.verticalCenter
        }
        iconColor:control.textColor
    }
    Item{
        id: d
        property var window: Window.window
    }
    onClicked: {
        if(menu.count !==0){
            var pos = control.mapToItem(null, 0, 0)
            var containerHeight = menu.count*36
            if(d.window.height>pos.y+control.height+containerHeight){
                menu.y = control.height
            }else if(pos.y>containerHeight){
                menu.y = -containerHeight
            }else{
                menu.y = d.window.height-(pos.y+containerHeight)
            }
            menu.open()
        }
    }
    FluMenu{
        id:menu
        modal:true
        // popup 宽度自适应内容（对齐 macOS 菜单随内容展开）：至少与按钮
        // 同宽；菜单项更宽时按菜单 implicitWidth 展开（FluMenu 背景最小宽
        // 150，可容纳"每页 50 个""导入壁纸文件夹…"等最长项），避免文本
        // 被固定按钮宽度裁剪。
        width: Math.max(control.width, menu.implicitWidth)
    }
}
