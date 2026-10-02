import QtQuick
import org.kde.plasma.plasmoid

WallpaperItem {
    id: root
    property bool initialized: false
    // WallpaperItem creates configuration after the package component is
    // constructed.  Do not instantiate the native display item until that
    // object exists; this avoids transient null bindings during package
    // installation and wallpaper replacement.
    readonly property bool configurationReady: root.configuration !== null

    Rectangle {
        anchors.fill: parent
        color: surfaceLoader.status === Loader.Ready
            ? surfaceLoader.item.clearColor
            : "black"
    }

    WindowModel {
        id: windowModel
        screenGeometry: Qt.rect(Screen.virtualX, Screen.virtualY,
                                Screen.width, Screen.height)
    }

    Loader {
        id: surfaceLoader
        anchors.fill: parent
        // Native QML module construction can require scene-graph resources;
        // asynchronous loading keeps Plasma's shell event loop responsive
        // while those resources become available after startup.
        asynchronous: true
        active: root.initialized && root.configurationReady
        source: "MirageSurface.qml"
        onLoaded: {
            item.configuredDisplayName = Qt.binding(function() {
                return root.configuration.DisplayName || "";
            });
            item.configuredSocketPath = Qt.binding(function() {
                return root.configuration.SocketPath || "";
            });
            item.configuredPointerForwarding = Qt.binding(function() {
                return root.configuration.MouseForward;
            });
            item.configuredWindowStateFlags = Qt.binding(function() {
                return windowModel.flags;
            });
            item.configuredRendererBackend = Qt.binding(function() {
                return root.configuration.RendererBackend;
            });
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8
        visible: root.configurationReady && root.configuration.ShowDiagnostics
            && surfaceLoader.status === Loader.Ready
        color: Qt.rgba(0, 0, 0, 0.62)
        radius: 4
        // Cap the overlay at the wallpaper width and wrap the text so the long
        // per-task rows dump stays readable instead of being clipped.
        width: Math.min(diagnostics.implicitWidth + 16, root.width - 16)
        height: diagnostics.implicitHeight + 12

        Text {
            id: diagnostics
            anchors.fill: parent
            anchors.margins: 6
            color: "white"
            font.family: "monospace"
            font.pixelSize: 12
            wrapMode: Text.Wrap
            text: {
                const item = surfaceLoader.item
                if (!item) return ""
                let value = item.rendererBackendName + "  "
                    + (item.connected ? "connected" : "disconnected")
                value += "\noutput=" + item.outputId + " generation=" + item.importedGeneration
                value += " frames=" + item.framesReceived
                value += " reconnects=" + item.reconnectAttempts
                value += " socketInode=" + item.socketInode
                value += " windowFlags=0x" + windowModel.flags.toString(16)
                value += " tasks=" + windowModel.taskCount
                value += " recalc=" + windowModel.debugRecomputeCount
                value += "\nscreen=" + windowModel.debugScreen
                value += "\nrows=" + windowModel.debugInfo
                if (item.lastError.length > 0) value += "\n" + item.lastError
                return value
            }
        }
    }

    Component.onCompleted: {
        root.initialized = true
        root.loading = false
    }
}
