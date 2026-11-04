// Copyright © 2026 王孝慈. All rights reserved.
import QtQuick
import QtQuick.Layouts

RowLayout {
    id: row
    property string label: ""
    property string setting: ""
    property var choices: []
    property string kind: "choice"
    property real minimum: 0
    property real maximum: 100
    property real step: 1
    property string suffix: ""
    property bool supported: true
    Layout.fillWidth: true
    spacing: 14
    MCheck {
        visible: row.kind === "check"
        Layout.fillWidth: true
        text: Theme.t(row.label)
        checked: Boolean(Store.preferences[row.setting])
        enabled: row.supported
        onClicked: Store.changePreference(row.setting, checked)
    }
    MText {
        visible: row.kind !== "check"
        text: Theme.t(row.label)
        Layout.fillWidth: true
    }
    MCombo {
        visible: row.kind === "choice"
        model: row.choices
        Layout.preferredWidth: 240
        currentIndex: Number(Store.preferences[row.setting] || 0)
        enabled: row.supported
        onActivated: Store.changePreference(row.setting, currentIndex)
    }
    MSlider {
        visible: row.kind === "slider"
        implicitWidth: 150
        from: row.minimum
        to: row.maximum
        stepSize: row.step
        value: Number(Store.preferences[row.setting])
        enabled: row.supported
        onMoved: Store.changePreference(row.setting, value)
    }
    MText {
        visible: row.kind === "slider"
        text: (row.maximum === 1 ? Math.round(Number(Store.preferences[row.setting]) * 100) : Math.round(Number(Store.preferences[row.setting]))) + row.suffix
        Layout.preferredWidth: 42
        horizontalAlignment: Text.AlignRight
    }
}
