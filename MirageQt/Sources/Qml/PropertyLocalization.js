.pragma library

var translations = null

function translationTable() {
    if (translations !== null) return translations

    translations = ({})
    try {
        var request = new XMLHttpRequest()
        request.open("GET", "qrc:/i18n/ui_zh-chs.json", false)
        request.send()
        if (request.status === 0 || (request.status >= 200 && request.status < 300)) {
            translations = JSON.parse(request.responseText)
        }
    } catch (error) {
        translations = ({})
    }
    return translations
}

function stripMarkup(value) {
    var s = String(value || "")
        .replace(/＜/g, "<")
        .replace(/＞/g, ">")
        .replace(/<br\s*\/?>/gi, "\n")
        .replace(/<[^>]*>/g, "")
        .replace(/[ \t]+/g, " ")
        .replace(/\n{3,}/g, "\n\n")
        .trim()
    return decodeHTMLEntities(s)
}

/**
 * HTML 实体解码（&amp; &lt; &gt; &quot; &apos; &nbsp; 与 &#N;/&#xN;），
 * 对齐 macOS WEHTML.decodeEntities。无法识别的实体原样保留。
 * @param {string} s - 原始字符串
 * @returns {string} 解码后的字符串
 */
function decodeHTMLEntities(s) {
    var text = String(s || "")
    if (text.indexOf("&") === -1) return text
    return text.replace(/&(#x?[0-9a-fA-F]+|[a-zA-Z]+);/g, function (match, entity) {
        switch (entity.toLowerCase()) {
        case "amp": return "&"
        case "lt": return "<"
        case "gt": return ">"
        case "quot": return "\""
        case "apos":
        case "#39": return "'"
        case "nbsp": return "\u00A0"
        }
        if (entity.charAt(0) === "#") {
            var code = (entity.charAt(1) === "x" || entity.charAt(1) === "X")
                ? parseInt(entity.substring(2), 16)
                : parseInt(entity.substring(1), 10)
            if (!isNaN(code) && code >= 0 && code <= 0x10FFFF) {
                try {
                    return String.fromCodePoint(code)
                } catch (error) {
                    return match
                }
            }
        }
        return match
    })
}

/**
 * 判断标签文本是否为富 HTML（含 img/a/table/center/iframe/video 标签），
 * 对齐 macOS WEHTML.isRich。
 * @param {string} raw - 原始标签文本
 * @returns {boolean} true 表示包含富内容标签
 */
function isRichHTML(raw) {
    var s = String(raw || "").replace(/＜/g, "<").replace(/＞/g, ">")
    return /<\s*(img|a|table|center|iframe|video)\b/i.test(s)
}

/**
 * 判断标签文本是否引用远程资源（img/iframe/video 的 http(s) 或 // src），
 * 对齐 macOS WEHTML.needsWebView。
 * @param {string} raw - 原始标签文本
 * @returns {boolean} true 表示需要 Web 视图渲染
 */
function needsWebViewHTML(raw) {
    var s = String(raw || "").replace(/＜/g, "<").replace(/＞/g, ">")
    return /<\s*(img|iframe|video)\b[^>]*\bsrc\s*=\s*["']?\s*(https?:|\/\/)/i.test(s)
}

function displayText(value) {
    var raw = stripMarkup(value)
    if (raw.length === 0) return raw

    var table = translationTable()
    if (table[raw] !== undefined) return stripMarkup(table[raw])
    if (raw.indexOf("ui_") !== 0) return raw

    var prefixes = [
        "ui_editor_script_snippet_",
        "ui_editor_properties_",
        "ui_browse_properties_",
        "ui_editor_general_",
        "ui_editor_effect_",
        "ui_editor_preset_",
        "ui_editor_",
        "ui_browse_",
        "ui_"
    ]
    var readable = raw
    for (var index = 0; index < prefixes.length; ++index) {
        if (readable.indexOf(prefixes[index]) === 0) {
            readable = readable.substring(prefixes[index].length)
            break
        }
    }
    return readable.replace(/_/g, " ").trim()
}

function propertyText(key, text) {
    var raw = String(text || "").trim()
    if (raw.length === 0) raw = String(key || "")
    return displayText(raw)
}
