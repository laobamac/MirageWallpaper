// Copyright © 2026 王孝慈. All rights reserved.
pragma Singleton
import QtQuick

QtObject {
    property var translations: ({})
    property var propertyTranslations: ({})
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
    // Loads the existing .strings tables and JSON additions by their CMake resource aliases.
    // Signature: loadTranslations(language: string) -> void; locale names follow the existing zh-Hans/zh-Hant/en files.
    function loadTranslations(language) {
        const systemLocale = Qt.locale().name;
        const locale = language === "system"
            ? (systemLocale.startsWith("zh")
               ? (systemLocale.includes("TW") || systemLocale.includes("HK") ? "zh-Hant" : "zh-Hans")
               : "en")
            : language;
        const request = new XMLHttpRequest();
        request.open("GET", "qrc:/MirageQt/Resources/" + locale + ".lproj/Localizable.strings", false);
        request.send();
        const table = ({ });
        const expression = /"((?:\\.|[^"\\])*)"\s*=\s*"((?:\\.|[^"\\])*)"\s*;/g;
        let match;
        while ((match = expression.exec(request.responseText)) !== null) {
            // Signature: decode(value: string) -> string; .strings supports these three escapes in this resource set.
            const decode = value => value.replace(/\\n/g, "\n").replace(/\\"/g, "\"").replace(/\\\\/g, "\\");
            table[decode(match[1])] = decode(match[2]);
        }
        const extraRequest = new XMLHttpRequest();
        extraRequest.open("GET", "qrc:/MirageQt/Resources/Translations.json", false);
        extraRequest.send();
        const extra = JSON.parse(extraRequest.responseText);
        for (const key in extra) {
            if (extra[key][locale]) table[key] = extra[key][locale];
        }
        translations = table;

        propertyTranslations = ({ });
        if (locale !== "en") {
            const propertyLocale = locale === "zh-Hant" ? "zh-cht" : "zh-chs";
            const propertyRequest = new XMLHttpRequest();
            propertyRequest.open("GET", "qrc:/MirageQt/Resources/ui_" + propertyLocale + ".json", false);
            propertyRequest.send();
            propertyTranslations = JSON.parse(propertyRequest.responseText);
        }
    }
    // Returns a translated UI string, falling back to the source key when absent.
    // Signature: t(key: string) -> string.
    function t(key) { return translations[key] === undefined ? key : translations[key]; }
    // Returns a translated wallpaper-property label or its source text.
    // Signature: p(key: string) -> string.
    function p(key) {
        if (propertyTranslations[key] !== undefined) return propertyTranslations[key];
        return translations[key] === undefined ? key : translations[key];
    }
    Component.onCompleted: loadTranslations(Store.language)
}
