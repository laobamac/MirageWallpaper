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

    @MainActor private static var pendingImports: [String: Task<AttributedString?, Never>] = [:]
    @MainActor private static var importSlots: [Task<AttributedString?, Never>?] = [nil, nil]
    @MainActor private static var nextImportSlot = 0

    @MainActor
    static func attributed(_ raw: String) async -> AttributedString? {
        let key = raw as NSString
        if let cached = attributedCache.object(forKey: key) { return cached.value }
        if let pending = pendingImports[raw] { return await pending.value }
        let slot = nextImportSlot
        nextImportSlot = (slot + 1) % importSlots.count
        let previous = importSlots[slot]
        let task = Task { @MainActor () -> AttributedString? in
            _ = await previous?.value
            let parsed: NSAttributedString? = await withCheckedContinuation { continuation in
                NSAttributedString.loadFromHTML(string: normalizeAngles(raw), options: [.timeout: 2.0]) {
                    value, _, _ in continuation.resume(returning: value)
                }
            }
            guard let parsed else { return nil }
            var result = AttributedString(parsed)
            for run in result.runs {
                result[run.range].foregroundColor = nil
                result[run.range].backgroundColor = nil
                result[run.range].font = nil
            }
            while let last = result.characters.last, last.isNewline || last == " " {
                result.removeSubrange(result.index(beforeCharacter: result.endIndex)..<result.endIndex)
            }
            attributedCache.setObject(Box(result), forKey: key)
            return result
        }
        pendingImports[raw] = task
        importSlots[slot] = task
        let result = await task.value
        pendingImports[raw] = nil
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

    private static let plainCache: NSCache<NSString, NSString> = {
        let cache = NSCache<NSString, NSString>()
        cache.countLimit = 1024
        return cache
    }()

    static func plain(_ raw: String) -> String {
        if let cached = plainCache.object(forKey: raw as NSString) { return cached as String }
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
        let result = s.trimmingCharacters(in: .whitespacesAndNewlines)
        plainCache.setObject(result as NSString, forKey: raw as NSString)
        return result
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
    @State private var attributed: AttributedString?
    @State private var loadedHTML: String?

    var body: some View {
        Group {
            if WEHTML.needsWebView(html) {
                RichHTMLWebViewHost(html: html)
            } else {
                Text(loadedHTML == html ? (attributed ?? AttributedString(WEHTML.plain(html)))
                     : AttributedString(WEHTML.plain(html)))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
        }
        .task(id: html) {
            guard !WEHTML.needsWebView(html) else { return }
            let result = await WEHTML.attributed(html)
            guard !Task.isCancelled else { return }
            attributed = result
            loadedHTML = html
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
