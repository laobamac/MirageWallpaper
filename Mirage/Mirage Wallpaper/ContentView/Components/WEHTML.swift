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

    @MainActor private static let imports = ImportQueue()

    @MainActor
    static func attributed(_ raw: String) async -> AttributedString? {
        await imports.value(for: raw)
    }

    @MainActor
    final class ImportQueue {
        private final class Request {
            let html: String
            var consumers: [UUID: CheckedContinuation<AttributedString?, Never>] = [:]
            var active = false

            init(_ html: String) { self.html = html }
        }

        private let importer: (String) async -> NSAttributedString?
        private let cache = NSCache<NSString, Box>()
        private var requests: [String: Request] = [:]
        private var waiting: [Request] = []
        private var activeCount = 0

        init(importer: @escaping (String) async -> NSAttributedString? = { raw in
            await withCheckedContinuation { continuation in
                NSAttributedString.loadFromHTML(string: normalizeAngles(raw), options: [.timeout: 2.0]) {
                    value, _, _ in continuation.resume(returning: value)
                }
            }
        }) {
            self.importer = importer
            cache.countLimit = 256
        }

        func value(for raw: String) async -> AttributedString? {
            guard !Task.isCancelled else { return nil }
            if let cached = cache.object(forKey: raw as NSString) { return cached.value }
            let token = UUID()
            return await withTaskCancellationHandler {
                await withCheckedContinuation { continuation in
                    guard !Task.isCancelled else { continuation.resume(returning: nil); return }
                    let request: Request
                    if let existing = requests[raw] {
                        request = existing
                    } else {
                        request = Request(raw)
                        requests[raw] = request
                        waiting.append(request)
                    }
                    request.consumers[token] = continuation
                    startWaiting()
                }
            } onCancel: {
                Task { @MainActor [weak self] in self?.cancel(raw, token: token) }
            }
        }

        private func cancel(_ raw: String, token: UUID) {
            guard let request = requests[raw] else { return }
            request.consumers.removeValue(forKey: token)?.resume(returning: nil)
            if request.consumers.isEmpty && !request.active {
                requests[raw] = nil
                waiting.removeAll { $0 === request }
            }
        }

        private func startWaiting() {
            while activeCount < 2, !waiting.isEmpty {
                let request = waiting.removeFirst()
                request.active = true
                activeCount += 1
                Task { @MainActor in
                    let parsed = await importer(request.html)
                    var result: AttributedString?
                    if !request.consumers.isEmpty, let parsed {
                        var value = AttributedString(parsed)
                        for run in value.runs {
                            value[run.range].foregroundColor = nil
                            value[run.range].backgroundColor = nil
                            value[run.range].font = nil
                        }
                        while let last = value.characters.last, last.isNewline || last == " " {
                            value.removeSubrange(value.index(beforeCharacter: value.endIndex)..<value.endIndex)
                        }
                        cache.setObject(Box(value), forKey: request.html as NSString)
                        result = value
                    }
                    requests[request.html] = nil
                    activeCount -= 1
                    let consumers = request.consumers.values
                    request.consumers.removeAll()
                    consumers.forEach { $0.resume(returning: result) }
                    startWaiting()
                }
            }
        }
    }

    private final class Box {
        let value: AttributedString
        init(_ value: AttributedString) { self.value = value }
    }

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
        var parser = PlainTextParser(normalizeAngles(raw))
        let result = parser.parse()
            .replacingOccurrences(of: "[ \\t]+", with: " ", options: .regularExpression)
            .replacingOccurrences(of: "\\n{3,}", with: "\n\n", options: .regularExpression)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        plainCache.setObject(result as NSString, forKey: raw as NSString)
        return result
    }

    static func decodeEntities(_ s: String) -> String {
        guard s.contains("&") else { return s }
        let bytes = Array(s.utf8)
        var output: [UInt8] = []
        output.reserveCapacity(bytes.count)
        var index = 0
        while index < bytes.count {
            if bytes[index] == 38, let entity = decodeEntity(in: bytes, at: index) {
                output.append(contentsOf: entity.text.utf8)
                index = entity.end
            } else {
                output.append(bytes[index])
                index += 1
            }
        }
        return String(decoding: output, as: UTF8.self)
    }

    private struct EntityTable: Decodable {
        let entities: [String: String]
    }

    private static let namedEntities: [String: String] = {
        guard let url = Bundle.main.url(forResource: "WEHTMLEntities", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let table = try? JSONDecoder().decode(EntityTable.self, from: data) else { return [:] }
        return table.entities
    }()

    private static let maximumEntityLength = namedEntities.keys.map { $0.utf8.count }.max() ?? 0

    private static let numericReplacements: [UInt32: UInt32] = [
        0x80: 0x20AC, 0x82: 0x201A, 0x83: 0x0192, 0x84: 0x201E, 0x85: 0x2026,
        0x86: 0x2020, 0x87: 0x2021, 0x88: 0x02C6, 0x89: 0x2030, 0x8A: 0x0160,
        0x8B: 0x2039, 0x8C: 0x0152, 0x8E: 0x017D, 0x91: 0x2018, 0x92: 0x2019,
        0x93: 0x201C, 0x94: 0x201D, 0x95: 0x2022, 0x96: 0x2013, 0x97: 0x2014,
        0x98: 0x02DC, 0x99: 0x2122, 0x9A: 0x0161, 0x9B: 0x203A, 0x9C: 0x0153,
        0x9E: 0x017E, 0x9F: 0x0178
    ]

    private static func isLetter(_ byte: UInt8) -> Bool {
        (65...90).contains(byte) || (97...122).contains(byte)
    }

    private static func isSpace(_ byte: UInt8) -> Bool {
        byte == 32 || (9...13).contains(byte) && byte != 11
    }

    private static func decodeEntity(in bytes: [UInt8], at start: Int) -> (text: String, end: Int)? {
        var index = start + 1
        guard index < bytes.count else { return nil }
        if bytes[index] == 35 {
            index += 1
            var radix: UInt32 = 10
            if index < bytes.count, bytes[index] == 120 || bytes[index] == 88 {
                radix = 16
                index += 1
            }
            let digitsStart = index
            var value: UInt32 = 0
            while index < bytes.count {
                let digit: UInt32
                switch bytes[index] {
                case 48...57: digit = UInt32(bytes[index] - 48)
                case 65...70 where radix == 16: digit = UInt32(bytes[index] - 65 + 10)
                case 97...102 where radix == 16: digit = UInt32(bytes[index] - 97 + 10)
                default: digit = radix
                }
                guard digit < radix else { break }
                value = min(0x110000, value * radix + digit)
                index += 1
            }
            guard index > digitsStart else { return nil }
            if index < bytes.count, bytes[index] == 59 { index += 1 }
            if value == 0 || value > 0x10FFFF || (0xD800...0xDFFF).contains(value) {
                value = 0xFFFD
            }
            let scalar = Unicode.Scalar(numericReplacements[value] ?? value) ?? "\u{FFFD}"
            return (String(scalar), index)
        }

        let nameStart = index
        let limit = min(bytes.count, nameStart + maximumEntityLength)
        var match: (text: String, end: Int)?
        while index < limit {
            let byte = bytes[index]
            guard isLetter(byte) || (48...57).contains(byte) || byte == 59 else { break }
            index += 1
            let name = String(decoding: bytes[nameStart..<index], as: UTF8.self)
            if let text = namedEntities[name] { match = (text, index) }
            if byte == 59 { break }
        }
        return match
    }

    private struct PlainTextParser {
        let bytes: [UInt8]
        var index = 0
        var output: [UInt8] = []

        private static let blockTags: Set<String> = [
            "br", "hr", "p", "div", "center", "h1", "h2", "h3", "h4", "h5", "h6",
            "li", "ul", "ol", "dl", "dt", "dd", "blockquote", "pre", "section",
            "article", "header", "footer", "table", "tr"
        ]

        init(_ text: String) {
            bytes = Array(text.utf8)
            output.reserveCapacity(bytes.count)
        }

        mutating func parse() -> String {
            while index < bytes.count {
                if bytes[index] == 60 {
                    if hasPrefix("<!--", at: index) {
                        skipComment()
                        continue
                    }
                    if hasPrefix("<!", at: index) || hasPrefix("<?", at: index) {
                        index = tagEnd(from: index + 2)
                        continue
                    }
                    if let tag = tag(at: index) {
                        index = tag.end
                        if !tag.closing && (tag.name == "style" || tag.name == "script") {
                            skipRawText(tag.name)
                        } else if Self.blockTags.contains(tag.name) {
                            output.append(10)
                        } else if tag.name == "td" || tag.name == "th" {
                            output.append(32)
                        }
                        continue
                    }
                }
                if bytes[index] == 38, let entity = decodeEntity(in: bytes, at: index) {
                    output.append(contentsOf: entity.text.utf8)
                    index = entity.end
                } else {
                    output.append(bytes[index])
                    index += 1
                }
            }
            return String(decoding: output, as: UTF8.self)
        }

        private func hasPrefix(_ prefix: String, at start: Int) -> Bool {
            bytes[start...].starts(with: prefix.utf8)
        }

        private mutating func skipComment() {
            index += 4
            if index < bytes.count, bytes[index] == 62 {
                index += 1
                return
            }
            if hasPrefix("->", at: index) {
                index += 2
                return
            }
            while index < bytes.count {
                if hasPrefix("-->", at: index) {
                    index += 3
                    return
                }
                if hasPrefix("--!>", at: index) {
                    index += 4
                    return
                }
                index += 1
            }
        }

        private mutating func skipRawText(_ name: String) {
            let closing = Array("</\(name)".utf8)
            while index < bytes.count {
                if bytes[index] == 60, index + closing.count < bytes.count {
                    var matches = true
                    for offset in closing.indices {
                        let actual = bytes[index + offset]
                        let lowercase: UInt8 = actual >= 65 && actual <= 90 ? actual + 32 : actual
                        if lowercase != closing[offset] {
                            matches = false
                            break
                        }
                    }
                    let next = bytes[index + closing.count]
                    if matches && (isSpace(next) || next == 47 || next == 62) {
                        index = tagEnd(from: index + closing.count)
                        return
                    }
                }
                index += 1
            }
        }

        private func tag(at start: Int) -> (name: String, closing: Bool, end: Int)? {
            var cursor = start + 1
            guard cursor < bytes.count else { return nil }
            let closing = bytes[cursor] == 47
            if closing { cursor += 1 }
            guard cursor < bytes.count, isLetter(bytes[cursor]) else { return nil }
            let nameStart = cursor
            while cursor < bytes.count, !isSpace(bytes[cursor]), bytes[cursor] != 47, bytes[cursor] != 62 {
                cursor += 1
            }
            let name = String(decoding: bytes[nameStart..<cursor], as: UTF8.self).lowercased()
            return (name, closing, tagEnd(from: cursor))
        }

        private enum AttributeState {
            case beforeName, name, afterName, beforeValue, unquotedValue, quotedValue(UInt8)
        }

        private func tagEnd(from start: Int) -> Int {
            var cursor = start
            var state = AttributeState.beforeName
            while cursor < bytes.count {
                let byte = bytes[cursor]
                if case .quotedValue(let quote) = state {
                    if byte == quote { state = .beforeName }
                } else {
                    if byte == 62 { return cursor + 1 }
                    switch state {
                    case .beforeName:
                        if !isSpace(byte) && byte != 47 { state = .name }
                    case .name:
                        if byte == 61 { state = .beforeValue }
                        else if isSpace(byte) { state = .afterName }
                        else if byte == 47 { state = .beforeName }
                    case .afterName:
                        if byte == 61 { state = .beforeValue }
                        else if !isSpace(byte) { state = byte == 47 ? .beforeName : .name }
                    case .beforeValue:
                        if byte == 34 || byte == 39 { state = .quotedValue(byte) }
                        else if !isSpace(byte) { state = .unquotedValue }
                    case .unquotedValue:
                        if isSpace(byte) { state = .beforeName }
                    case .quotedValue:
                        break
                    }
                }
                cursor += 1
            }
            return cursor
        }
    }
}

// Renders rich WE HTML with a WKWebView: transparent, follows system
// colors/fonts, images fit width and load async, links open in the browser.
// Self-scrolling is disabled; height is measured via JS and the wheel is
// forwarded to the enclosing ScrollView.
struct RichHTMLText: View {
    let html: String
    @Environment(\.mirageContentActive) private var isActive
    @State private var attributed: AttributedString?
    @State private var loadedHTML: String?

    var body: some View {
        Group {
            if isActive && WEHTML.needsWebView(html) {
                RichHTMLWebViewHost(html: html)
            } else {
                Text(loadedHTML == html ? (attributed ?? AttributedString(WEHTML.plain(html)))
                     : AttributedString(WEHTML.plain(html)))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
        }
        .task(id: isActive ? html : nil) {
            guard isActive, !WEHTML.needsWebView(html) else { return }
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
        config.userContentController.add(context.coordinator,
            contentWorld: .defaultClient, name: "mirageLabelSize")
        let webView = PassThroughWebView(frame: .zero, configuration: config)
        webView.navigationDelegate = context.coordinator
        webView.setValue(false, forKey: "drawsBackground")
        webView.enclosingScrollView?.hasVerticalScroller = false
        return webView
    }

    func updateNSView(_ webView: WKWebView, context: Context) {
        context.coordinator.parent = self
        guard context.coordinator.loadedHTML != html else { return }
        context.coordinator.loadedHTML = html
        let generation = UUID().uuidString
        context.coordinator.generation = generation
        context.coordinator.publish(height: 24, generation: generation)
        let controller = webView.configuration.userContentController
        controller.removeAllUserScripts()
        controller.addUserScript(WKUserScript(source: Self.measurementScript(generation: generation),
            injectionTime: .atDocumentEnd, forMainFrameOnly: true, in: .defaultClient))
        webView.loadHTMLString(Self.wrap(html), baseURL: nil)
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    static func dismantleNSView(_ webView: WKWebView, coordinator: Coordinator) {
        coordinator.generation = nil
        coordinator.publication?.cancel()
        coordinator.publication = nil
        webView.evaluateJavaScript("window.__mirageLabelCleanup?.()", in: nil,
                                  in: .defaultClient, completionHandler: nil)
        webView.stopLoading()
        webView.navigationDelegate = nil
        webView.configuration.userContentController.removeScriptMessageHandler(
            forName: "mirageLabelSize", contentWorld: .defaultClient)
        webView.configuration.userContentController.removeAllUserScripts()
    }

    private static func measurementScript(generation: String) -> String {
        """
        (() => {
            const content = document.getElementById('mirage-label-content');
            if (!content) return;
            let scheduled = false;
            let disposed = false;
            let previous = -1;
            const measure = () => {
                if (disposed || scheduled) return;
                scheduled = true;
                Promise.resolve().then(() => {
                    scheduled = false;
                    if (disposed) return;
                    const height = Math.max(1, Math.ceil(Math.max(
                        content.getBoundingClientRect().height, content.scrollHeight)));
                    if (height === previous) return;
                    previous = height;
                    window.webkit.messageHandlers.mirageLabelSize.postMessage({
                        generation: '\(generation)', height
                    });
                });
            };
            const resize = new ResizeObserver(measure);
            const mutation = new MutationObserver(measure);
            resize.observe(content);
            mutation.observe(content, {
                subtree: true, childList: true, attributes: true, characterData: true
            });
            document.addEventListener('load', measure, true);
            window.addEventListener('resize', measure);
            window.__mirageLabelCleanup = () => {
                disposed = true;
                resize.disconnect();
                mutation.disconnect();
                document.removeEventListener('load', measure, true);
                window.removeEventListener('resize', measure);
            };
            if (document.fonts) document.fonts.ready.then(measure);
            measure();
        })();
        """
    }

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
        #mirage-label-content { display: flow-root; width: 100%; }
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
        </style></head><body><div id="mirage-label-content">\(normalized)</div></body></html>
        """
    }

    final class Coordinator: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
        var parent: RichHTMLWebView
        var loadedHTML: String?
        var generation: String?
        var publication: DispatchWorkItem?
        init(_ parent: RichHTMLWebView) { self.parent = parent }

        func userContentController(_ userContentController: WKUserContentController,
                                   didReceive message: WKScriptMessage) {
            guard message.frameInfo.isMainFrame,
                  let payload = message.body as? [String: Any],
                  let generation = payload["generation"] as? String,
                  let height = payload["height"] as? Double,
                  height.isFinite, height > 0 else { return }
            publish(height: CGFloat(height), generation: generation)
        }

        func publish(height: CGFloat, generation: String) {
            guard self.generation == generation else { return }
            publication?.cancel()
            let work = DispatchWorkItem { [weak self] in
                guard let self, self.generation == generation else { return }
                self.publication = nil
                if abs(height - self.parent.height) > 0.5 { self.parent.height = height }
            }
            publication = work
            DispatchQueue.main.async(execute: work)
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
