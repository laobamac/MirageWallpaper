import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    id: root
    spacing: Kirigami.Units.largeSpacing

    // Plasma 6 assigns these objects before opening the wallpaper settings
    // page.  Declaring the documented properties keeps the assignment on the
    // root object instead of producing binding errors in plasmashell.
    property var configDialog
    property var wallpaperConfiguration: wallpaper.configuration
    property var parentLayout

    property string cfg_DisplayName
    property string cfg_SocketPath
    property bool cfg_MouseForward
    property bool cfg_ShowDiagnostics
    property int cfg_RendererBackend

    Kirigami.FormLayout {
        Layout.fillWidth: true

        QQC2.TextField {
            Kirigami.FormData.label: qsTr("Display name:")
            placeholderText: qsTr("Automatic")
            text: cfg_DisplayName
            onTextChanged: cfg_DisplayName = text
        }

        QQC2.TextField {
            Kirigami.FormData.label: qsTr("Broker socket:")
            placeholderText: qsTr("Automatic")
            text: cfg_SocketPath
            onTextChanged: cfg_SocketPath = text
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: qsTr("Forward pointer events")
            checked: cfg_MouseForward
            onToggled: cfg_MouseForward = checked
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: qsTr("Show diagnostics")
            checked: cfg_ShowDiagnostics
            onToggled: cfg_ShowDiagnostics = checked
        }

        QQC2.ComboBox {
            Kirigami.FormData.label: qsTr("Render backend:")
            model: [qsTr("Automatic"), qsTr("OpenGL"), qsTr("Vulkan")]
            currentIndex: cfg_RendererBackend
            onActivated: cfg_RendererBackend = currentIndex
        }
    }

    Item {
        Layout.fillHeight: true
    }
}
