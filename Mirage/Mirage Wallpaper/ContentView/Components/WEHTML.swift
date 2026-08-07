//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import WebKit

// WE labels are HTML. Rich content (images / links / tables) is rendered
// faithfully with WKWebView; everything else is flattened to plain text so most
// rows stay native and cheap.
enum WEHTML {
    static func isRich(_ raw: String) -> Bool {
        let s = raw.replacingOccurrences(of: "＜", with: "<").replacingOccurrences(of: "＞", with: ">")
        return s.range(of: "<\\s*(img|a|table|center|iframe|video)\\b",
                       options: [.regularExpression, .caseInsensitive]) != nil
    }

    /// Rich labels that still need an out-of-process web view.
    ///
    /// `NSAttributedString`'s HTML importer resolves referenced resources
    /// synchronously on the calling (main) thread, so a label pointing at a
    /// remote image would beachball the settings panel. Those keep the web
    /// view; everything else — formatting, links, tables, local images —
    /// renders natively and costs no WebKit content process at all.
    static func needsWebView(_ raw: String) -> Bool {
        let s = normalizeAngles(raw)
        return s.range(of: "<\\s*(img|iframe|video)\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*(https?:|//)",
                       options: [.regularExpression, .caseInsensitive]) != nil
    }

    /// Parse a rich label into an `AttributedString`.
    ///
    /// Imported fonts and colours are dropped so the surrounding SwiftUI
    /// environment styles the text, which also keeps it legible in dark mode —
    /// the importer otherwise hardcodes Times at black.
    static func attributed(_ raw: String) -> AttributedString? {
        let key = raw as NSString
        if let cached = attributedCache.object(forKey: key) { return cached.value }

        let normalized = normalizeAngles(raw)
        guard let data = normalized.data(using: .utf8) else { return nil }
        guard let parsed = try? NSAttributedString(
            data: data,
            options: [
                .documentType: NSAttributedString.DocumentType.html,
                .characterEncoding: String.Encoding.utf8.rawValue,
            ],
            documentAttributes: nil
        ) else { return nil }

        var result = AttributedString(parsed)
        for run in result.runs {
            result[run.range].foregroundColor = nil
            result[run.range].backgroundColor = nil
            result[run.range].font = nil
        }
        // Trailing newlines are an artefact of block-level tags closing.
        while let last = result.characters.last, last.isNewline || last == " " {
            result.removeSubrange(result.index(beforeCharacter: result.endIndex)..<result.endIndex)
        }
        attributedCache.setObject(Box(result), forKey: key)
        return result
    }

    private final class Box {
        let value: AttributedString
        init(_ value: AttributedString) { self.value = value }
    }

    // Parsing HTML is not cheap and SwiftUI re-evaluates bodies freely, so the
    // result is memoised per source string.
    private static let attributedCache: NSCache<NSString, Box> = {
        let cache = NSCache<NSString, Box>()
        cache.countLimit = 256
        return cache
    }()

    private static func normalizeAngles(_ raw: String) -> String {
        raw.replacingOccurrences(of: "＜", with: "<")
            .replacingOccurrences(of: "＞", with: ">")
    }

    static func plain(_ raw: String) -> String {
        var s = raw
            .replacingOccurrences(of: "＜", with: "<")
            .replacingOccurrences(of: "＞", with: ">")
        func rx(_ pattern: String, _ rep: String) {
            s = s.replacingOccurrences(of: pattern, with: rep,
                                       options: [.regularExpression, .caseInsensitive])
        }
        rx("<\\s*br\\s*/?>", "\n")
        rx("<\\s*/?\\s*(p|div|center)\\s*>", "\n")
        rx("<[^>]*>", "")
        rx("<\\s*/?\\s*[a-zA-Z][^<]*$", "")            // truncated trailing tag
        rx("(?m)^[ \\t]*/?(?:center|big|small|strong|font|span|div|sub|sup|b|i|u|p|a)[ \\t]*>[ \\t]*$", "")
        s = decodeEntities(s)
        rx("[ \\t]+", " ")
        rx("\\n{3,}", "\n\n")
        return s.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    static func decodeEntities(_ s: String) -> String {
        guard s.contains("&") else { return s }
        var out = ""
        out.reserveCapacity(s.count)
        var i = s.startIndex
        while i < s.endIndex {
            let c = s[i]
            if c == "&", let semi = s[i...].firstIndex(of: ";") {
                let entity = String(s[s.index(after: i)..<semi])
                if let decoded = decodeEntity(entity) {
                    out.append(decoded)
                    i = s.index(after: semi)
                    continue
                }
            }
            out.append(c)
            i = s.index(after: i)
        }
        return out
    }

    private static func decodeEntity(_ e: String) -> Character? {
        switch e.lowercased() {
        case "amp": return "&"
        case "lt": return "<"
        case "gt": return ">"
        case "quot": return "\""
        case "apos", "#39": return "'"
        case "nbsp": return "\u{00A0}"
        default: break
        }
        if e.hasPrefix("#x") || e.hasPrefix("#X") {
            if let v = UInt32(e.dropFirst(2), radix: 16), let scalar = Unicode.Scalar(v) {
                return Character(scalar)
            }
        } else if e.hasPrefix("#") {
            if let v = UInt32(e.dropFirst()), let scalar = Unicode.Scalar(v) {
                return Character(scalar)
            }
        }
        return nil
    }
}

// Renders rich WE HTML with a WKWebView: transparent, follows system
// colors/fonts, images fit width and load async, links open in the browser.
// Self-scrolling is disabled; height is measured via JS and the wheel is
// forwarded to the enclosing ScrollView.
struct RichHTMLText: View {
    let html: String

    var body: some View {
        // Only labels referencing remote resources still pay for a web view.
        // A property panel with fifteen rich labels used to spin up fifteen
        // WebKit content processes; now that is normally zero.
        if WEHTML.needsWebView(html) {
            RichHTMLWebViewHost(html: html)
        } else if let attributed = WEHTML.attributed(html) {
            Text(attributed)
                .frame(maxWidth: .infinity, alignment: .leading)
                .textSelection(.enabled)
        } else {
            Text(WEHTML.plain(html))
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

private struct RichHTMLWebViewHost: View {
    let html: String
    @State private var height: CGFloat = 24

    var body: some View {
        RichHTMLWebView(html: html, height: $height)
            .frame(height: height)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private final class PassThroughWebView: WKWebView {
    override func scrollWheel(with event: NSEvent) {
        nextResponder?.scrollWheel(with: event)
    }
}

private struct RichHTMLWebView: NSViewRepresentable {
    let html: String
    @Binding var height: CGFloat

    func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        let webView = PassThroughWebView(frame: .zero, configuration: config)
        webView.navigationDelegate = context.coordinator
        webView.setValue(false, forKey: "drawsBackground")
        webView.enclosingScrollView?.hasVerticalScroller = false
        return webView
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        guard context.coordinator.loadedHTML != html else { return }
        context.coordinator.loadedHTML = html
        webView.loadHTMLString(Self.wrap(html), baseURL: nil)
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    private static func wrap(_ body: String) -> String {
        let normalized = body
            .replacingOccurrences(of: "＜", with: "<")
            .replacingOccurrences(of: "＞", with: ">")
        return """
        <!DOCTYPE html><html><head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>
        :root { color-scheme: light dark; }
        html, body { margin:0; padding:0; background:transparent; }
        /* Property labels are display-only: retain link clicks while disabling
           WebKit's default text selection and drag sources. */
        html, body, body * {
            -webkit-user-select: none;
            user-select: none;
            -webkit-user-drag: none;
        }
        body {
            font: -apple-system-body, system-ui;
            font-size: 13px; line-height: 1.45;
            color: -apple-system-label;
            word-break: break-word; overflow-wrap: anywhere;
            overflow: hidden;
        }
        a { color: -apple-system-blue; text-decoration: none; }
        a:hover { text-decoration: underline; }
        img {
            max-width: 100%; height: auto; border-radius: 6px; display: block; margin: 4px 0;
            -webkit-user-drag: none;
        }
        big { font-size: 1.2em; }
        center { text-align: center; }
        p { margin: 4px 0; }
        table { max-width: 100%; }
        </style></head><body>\(normalized)</body></html>
        """
    }

    final class Coordinator: NSObject, WKNavigationDelegate {
        let parent: RichHTMLWebView
        var loadedHTML: String?
        init(_ parent: RichHTMLWebView) { self.parent = parent }

        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            measure(webView)
            // Remote images change height after decode; re-measure once.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak webView] in
                guard let webView else { return }
                self.measure(webView)
            }
        }

        private func measure(_ webView: WKWebView) {
            let js = "Math.max(document.body.scrollHeight, document.documentElement.scrollHeight)"
            webView.evaluateJavaScript(js) { result, _ in
                let h: CGFloat?
                if let v = result as? CGFloat { h = v }
                else if let n = result as? NSNumber { h = CGFloat(truncating: n) }
                else { h = nil }
                if let h, h > 0, abs(h - self.parent.height) > 0.5 {
                    DispatchQueue.main.async { self.parent.height = h }
                }
            }
        }

        func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
                     decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
            if navigationAction.navigationType == .linkActivated, let url = navigationAction.request.url {
                NSWorkspace.shared.open(url)
                decisionHandler(.cancel)
                return
            }
            decisionHandler(.allow)
        }
    }
}
