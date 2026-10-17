import QtQuick
import "MirageDisplayEmbed" as Mirage

Mirage.MirageDisplayItem {
    id: display

    property string configuredDisplayName: ""
    property string configuredSocketPath: ""
    property bool configuredPointerForwarding: true
    property int configuredWindowStateFlags: 0
    property int configuredRendererBackend: 0
    readonly property string rendererBackendName: rendererBackend === Mirage.MirageDisplayItem.BackendVulkan
        ? "vulkan"
        : (rendererBackend === Mirage.MirageDisplayItem.BackendOpenGLEGL ? "egl" : "none")

    readonly property string screenIdentity: {
        const manufacturer = (Screen.manufacturer || "").trim()
        const model = (Screen.model || "").trim()
        const serial = (Screen.serialNumber || "").trim()
        const connector = (Screen.name || "").trim()
        if (serial.length > 0)
            return [manufacturer, model, serial].join("|")
        return [manufacturer, model, connector].join("|")
    }

    socketPath: configuredSocketPath.length > 0 ? configuredSocketPath : defaultSocketPath
    outputStableId: "kde:" + screenIdentity
    outputName: configuredDisplayName.length > 0
        ? configuredDisplayName
        : (Screen.model || Screen.name || "KDE wallpaper")
    physicalWidth: Math.max(1, Math.round(width * Screen.devicePixelRatio))
    physicalHeight: Math.max(1, Math.round(height * Screen.devicePixelRatio))
    logicalWidth: Math.max(1, Math.round(width))
    logicalHeight: Math.max(1, Math.round(height))
    scale120: Math.max(1, Math.round(Screen.devicePixelRatio * 120))
    refreshMhz: Math.max(1, Math.round((Screen.refreshRate || 60) * 1000))
    outputTransform: {
        switch (Screen.orientation) {
        case Qt.PortraitOrientation: return Mirage.MirageDisplayItem.Transform90;
        case Qt.InvertedLandscapeOrientation: return Mirage.MirageDisplayItem.Transform180;
        case Qt.InvertedPortraitOrientation: return Mirage.MirageDisplayItem.Transform270;
        default: return Mirage.MirageDisplayItem.TransformNormal;
        }
    }
    pointerForwarding: configuredPointerForwarding
    windowStateFlags: configuredWindowStateFlags
    rendererBackendPreference: configuredRendererBackend
}
