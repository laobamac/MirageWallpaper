import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

ColumnLayout {
    spacing: Kirigami.Units.largeSpacing

    property string cfg_DisplayName
    property string cfg_SocketPath
    property bool cfg_MouseForward
    property bool cfg_ShowDiagnostics

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
    }

    Item {
        Layout.fillHeight: true
    }
}
