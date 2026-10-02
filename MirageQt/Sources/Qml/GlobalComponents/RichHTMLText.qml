import QtQuick
import QtWebEngine
import FluentUI
import "../PropertyLocalization.js" as PropertyLocalization

// 富 HTML 标签文本（对齐 macOS ContentView/Components/WEHTML.swift 的
// RichHTMLText）：仅引用远程资源（img/iframe/video 的 http src）时用
// WebEngineView 渲染（透明背景、禁用 JS、高度随内容自适应）；否则含
// 富内容标签（a/table/center 等）时用 Text.richText；其余走纯文本。
// 链接点击用系统浏览器打开（对齐 macOS decidePolicyFor linkActivated）。
Item {
    id: root

    property string html: ""
    property int wrapMode: Text.WordWrap

    // 判定结果缓存（html 不变时不重算）。
    readonly property bool __needsWeb: PropertyLocalization.needsWebViewHTML(root.html)
    readonly property bool __rich: PropertyLocalization.isRichHTML(root.html)

    // 远程资源路径时高度取 WebEngineView 内容高度（经 Loader.item 转发：
    // 内联 Component 内的 id 不在根 Item 作用域，不能直接引用 webViewItem；
    // Loader 未激活时 item 为 null，回退 24）。
    implicitHeight: root.__needsWeb
        ? Math.max(webLoader.item ? webLoader.item.contentsHeight : 24, 24)
        : textItem.implicitHeight
    implicitWidth: root.__needsWeb ? 200 : textItem.implicitWidth

    // 纯文本 / 本地富文本路径。
    Text {
        id: textItem
        visible: !root.__needsWeb
        anchors.fill: parent
        // 富标签渲染原始 HTML（Qt richText 支持 img/a/table 等子集），
        // 否则用 stripMarkup 剥离标签 + 解码实体为纯文本。
        text: root.__rich ? root.html : PropertyLocalization.stripMarkup(root.html)
        textFormat: root.__rich ? Text.RichText : Text.AutoText
        wrapMode: root.wrapMode
        color: FluTheme.fontPrimaryColor
        linkColor: FluTheme.primaryColor
        onLinkActivated: Qt.openUrlExternally(link)
    }

    // 远程资源路径：仅在此类标签出现时才实例化 WebEngineView，
    // 避免属性面板为普通标签启动 Web 内容进程（对齐 macOS 的分流）。
    Loader {
        id: webLoader
        anchors.fill: parent
        active: root.__needsWeb
        sourceComponent: Component {
            Item {
                id: webHost
                // 内容高度转发：根 Item 的 implicitHeight 经 Loader.item
                // 读取（见根注释），WebEngineView 内容加载完成前为 0。
                property int contentsHeight: webViewItem.contentsSize.height
                WebEngineView {
                    id: webViewItem
                    anchors.fill: parent
                    backgroundColor: "transparent"
                    settings.javascriptEnabled: false
                    // 链接点击交给系统浏览器，其余导航放行（对齐 macOS
                    // decidePolicyFor 的 linkActivated 分支）。
                    onNavigationRequested: function (request) {
                        // Qt 6.8 枚举：WebEngineNavigationRequest 下的
                        // NavigationType（LinkClickedNavigation 等），
                        // 旧 API WebEngineNavigationType.LinkClicked 已移除。
                        if (request.navigationType === WebEngineNavigationRequest.LinkClickedNavigation) {
                            request.action = WebEngineNavigationRequest.IgnoreRequest;
                            Qt.openUrlExternally(request.url);
                        } else {
                            request.action = WebEngineNavigationRequest.AcceptRequest;
                        }
                    }
                    // 组件（重建后）用当前 root.html 加载（对齐 updateNSView 的
                    // loadedHTML 判断）。
                    Component.onCompleted: loadHtml(root.html, "")
                }
            }
        }
    }
    // html 变化时重建 WebEngineView 以加载新内容（对齐 updateNSView 中
    // loadedHTML != html 时重新 loadHTMLString）。
    onHtmlChanged: {
        if (root.__needsWeb) {
            webLoader.active = false;
            webLoader.active = true;
        }
    }
}
