// Copyright © 2026 王孝慈. All rights reserved.
pragma Singleton
import QtQuick

QtObject {
    readonly property bool dark: Store.preferences.appearance === 1 || (Store.preferences.appearance === 2 && Qt.styleHints.colorScheme === Qt.Dark)
    readonly property color window: dark ? "#252527" : "#ededee"
    readonly property color panel: dark ? "#2c2c2e" : "#f5f5f6"
    readonly property color field: dark ? "#202022" : "#ffffff"
    readonly property color text: dark ? "#f2f2f3" : "#242426"
    readonly property color secondary: dark ? "#a7a7ac" : "#77777d"
    readonly property color line: dark ? "#454548" : "#d6d6d9"
    readonly property color accent: "#007aff"
    readonly property color hover: dark ? "#454549" : "#e0e0e4"
    readonly property color red: "#ff453a"
    readonly property color orange: "#ff9f0a"
    function t(key) {
        const language = bridge.language;
        return bridge.text(key);
    }
    function p(key) {
        const language = bridge.language;
        return bridge.propertyText(key);
    }
}
