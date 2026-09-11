//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Foundation
import SwiftUI
import UniformTypeIdentifiers

final class MirageLogService: ObservableObject {
    static let shared = MirageLogService()

    @Published private(set) var visibleText = ""

    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.logging", qos: .utility)
    private let maximumVisibleCharacters = 200_000
    private let startLock = NSLock()
    private var recentLines: [String] = []
    private var recentCharacterCount = 0
    private var displayGeneration: UInt64 = 0
    private var queueDisplayGeneration: UInt64 = 0
    private var isDisplaying = false
    private var publicationScheduled = false
    private var bufferVersion: UInt64 = 0
    private var publishedVersion: UInt64?
    private let timestampFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        return formatter
    }()
    private var sessionURL: URL?
    private var sessionHandle: FileHandle?
    private var stdoutPipe: Pipe?
    private var stderrPipe: Pipe?
    private var originalStdout: FileHandle?
    private var originalStderr: FileHandle?
    private var automaticSaveURL: URL?
    private var started = false
    private let logDirectory: URL?
    private let capturesStandardStreams: Bool

    init(logDirectory: URL? = nil, capturesStandardStreams: Bool = true) {
        self.logDirectory = logDirectory
        self.capturesStandardStreams = capturesStandardStreams
    }

    func start() {
        startLock.lock()
        guard !started else {
            startLock.unlock()
            return
        }
        started = true
        queue.async { [self] in
            let directory = logDirectory ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appending(path: "Mirage/Logs", directoryHint: .isDirectory)
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            removeExpiredSessions(in: directory)
            let url = directory.appending(path: "Session-\(UUID().uuidString).log")
            FileManager.default.createFile(atPath: url.path, contents: nil)
            sessionURL = url
            sessionHandle = try? FileHandle(forWritingTo: url)
        }
        startLock.unlock()
        if capturesStandardStreams { captureStandardStreams() }
        append("Mirage logging session started", source: "app")
    }

    func append(_ message: String, source: String = "app") {
        startLock.lock()
        let enabled = started
        startLock.unlock()
        guard enabled else { return }
        queue.async { [self] in
            let sanitized = Self.redact(message)
            guard !sanitized.isEmpty else { return }
            let stamp = timestampFormatter.string(from: Date())
            let normalized = sanitized.hasSuffix("\n") ? sanitized : sanitized + "\n"
            let line = "[\(stamp)] [\(source)] \(normalized)"
            if let data = line.data(using: .utf8) {
                try? sessionHandle?.write(contentsOf: data)
            }
            let tail = String(line.suffix(maximumVisibleCharacters))
            recentLines.append(tail)
            recentCharacterCount += tail.utf16.count
            while recentCharacterCount > maximumVisibleCharacters, recentLines.count > 1 {
                recentCharacterCount -= recentLines.removeFirst().utf16.count
            }
            bufferVersion &+= 1
            schedulePublication()
        }
    }

    func setDisplaying(_ displayed: Bool) {
        displayGeneration &+= 1
        let generation = displayGeneration
        queue.async { [self] in
            queueDisplayGeneration = generation
            isDisplaying = displayed
            if displayed {
                publishedVersion = nil
                publishSnapshot()
            }
        }
    }

    private func schedulePublication() {
        guard isDisplaying, !publicationScheduled else { return }
        publicationScheduled = true
        queue.asyncAfter(deadline: .now() + 0.25) { [self] in
            publicationScheduled = false
            publishSnapshot()
        }
    }

    private func publishSnapshot() {
        guard isDisplaying, publishedVersion != bufferVersion else { return }
        let snapshot = recentLines.joined()
        let generation = queueDisplayGeneration
        publishedVersion = bufferVersion
        DispatchQueue.main.async { [weak self] in
            guard let self, self.displayGeneration == generation,
                  self.visibleText != snapshot else { return }
            self.visibleText = snapshot
        }
    }

    func export(to url: URL) {
        queue.async { [self] in
            try? sessionHandle?.synchronize()
            guard let sessionURL, let data = try? Data(contentsOf: sessionURL) else { return }
            try? data.write(to: url, options: .atomic)
        }
    }

    func saveAutomaticallyInBackground() {
        queue.async { [self] in _ = saveAutomaticallyOnQueue() }
    }

    @discardableResult
    func saveAutomatically() -> URL? {
        queue.sync { saveAutomaticallyOnQueue() }
    }

    private func saveAutomaticallyOnQueue() -> URL? {
        do {
            try sessionHandle?.synchronize()
            guard let sessionURL else { return nil }
            let destination: URL
            if let automaticSaveURL {
                destination = automaticSaveURL
            } else {
                let desktop = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]
                let directory = desktop.appending(path: "MirageLogs", directoryHint: .isDirectory)
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                let base = "Mirage-\(Self.fileTimestamp())"
                var candidate = directory.appending(path: "\(base).log")
                var suffix = 2
                while FileManager.default.fileExists(atPath: candidate.path) {
                    candidate = directory.appending(path: "\(base)-\(suffix).log")
                    suffix += 1
                }
                automaticSaveURL = candidate
                destination = candidate
            }
            let data = try Data(contentsOf: sessionURL)
            try data.write(to: destination, options: .atomic)
            return destination
        } catch {
            return nil
        }
    }

    private func captureStandardStreams() {
        fflush(nil)
        let stdoutCopy = dup(STDOUT_FILENO)
        let stderrCopy = dup(STDERR_FILENO)
        if stdoutCopy >= 0 {
            originalStdout = FileHandle(fileDescriptor: stdoutCopy, closeOnDealloc: true)
        }
        if stderrCopy >= 0 {
            originalStderr = FileHandle(fileDescriptor: stderrCopy, closeOnDealloc: true)
        }

        let stdoutPipe = Pipe()
        let stderrPipe = Pipe()
        self.stdoutPipe = stdoutPipe
        self.stderrPipe = stderrPipe
        dup2(stdoutPipe.fileHandleForWriting.fileDescriptor, STDOUT_FILENO)
        dup2(stderrPipe.fileHandleForWriting.fileDescriptor, STDERR_FILENO)

        stdoutPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else {
                handle.readabilityHandler = nil
                return
            }
            self?.consume(data, source: "stdout", original: self?.originalStdout)
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else {
                handle.readabilityHandler = nil
                return
            }
            self?.consume(data, source: "stderr", original: self?.originalStderr)
        }
    }

    private func removeExpiredSessions(in directory: URL) {
        guard let expiration = Calendar.current.date(byAdding: .day, value: -7, to: Date()),
              let urls = try? FileManager.default.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: [.contentModificationDateKey],
                options: [.skipsHiddenFiles]
              ) else { return }
        for url in urls where url.lastPathComponent.hasPrefix("Session-") {
            guard let values = try? url.resourceValues(forKeys: [.contentModificationDateKey]),
                  let modificationDate = values.contentModificationDate,
                  modificationDate < expiration else { continue }
            try? FileManager.default.removeItem(at: url)
        }
    }

    private func consume(_ data: Data, source: String, original: FileHandle?) {
        try? original?.write(contentsOf: data)
        guard let text = String(data: data, encoding: .utf8) else { return }
        append(text, source: source)
    }

    private static let redactions: [(NSRegularExpression, String)] = {
        let replacements = [
            ("(?i)(key|api[_-]?key|token|access[_-]?token|refresh[_-]?token|password|passwd|steamguard|guard[_-]?code)(\\s*[=:]\\s*)[^\\s&\\\"']+", "$1$2<redacted>"),
            ("(?i)([?&](?:key|api[_-]?key|token|access_token|password)=)[^&\\s]+", "$1<redacted>"),
            ("(?<![A-Fa-f0-9])[A-Fa-f0-9]{32}(?![A-Fa-f0-9])", "<redacted>")
        ]
        return replacements.compactMap { pattern, template in
            guard let expression = try? NSRegularExpression(pattern: pattern) else { return nil }
            return (expression, template)
        }
    }()

    private static func redact(_ value: String) -> String {
        var result = value
        for (expression, template) in redactions {
            result = expression.stringByReplacingMatches(
                in: result, range: NSRange(result.startIndex..., in: result), withTemplate: template)
        }
        return result
    }

    private static func fileTimestamp() -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        return formatter.string(from: Date())
    }
}

final class DeveloperLogWindowController: NSWindowController, NSWindowDelegate {
    private let service: MirageLogService

    init(service: MirageLogService = .shared) {
        self.service = service
        let view = DeveloperLogView(service: service)
        let controller = NSHostingController(rootView: view)
        let window = NSWindow(contentViewController: controller)
        window.setContentSize(NSSize(width: 820, height: 520))
        window.contentMinSize = NSSize(width: 560, height: 320)
        window.styleMask = [.titled, .closable, .miniaturizable, .resizable]
        window.title = L("Mirage 开发日志")
        window.isReleasedWhenClosed = false
        window.setFrameAutosaveName("DeveloperLogWindow")
        super.init(window: window)
        window.delegate = self
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func showWindow(_ sender: Any?) {
        super.showWindow(sender)
        service.setDisplaying(true)
    }

    func windowWillClose(_ notification: Notification) {
        service.setDisplaying(false)
        service.saveAutomaticallyInBackground()
    }

    func windowDidChangeOcclusionState(_ notification: Notification) {
        service.setDisplaying(
            window?.isVisible == true && window?.occlusionState.contains(.visible) == true)
    }

    func refreshLocalization() {
        window?.title = L("Mirage 开发日志")
    }
}

private struct DeveloperLogView: View {
    @ObservedObject var service: MirageLogService

    var body: some View {
        VStack(spacing: 0) {
            DeveloperLogTextView(text: service.visibleText)
            Divider()
            HStack {
                Text(L("实时日志"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button(L("存储日志…")) {
                    let panel = NSSavePanel()
                    panel.allowedContentTypes = [.plainText]
                    panel.nameFieldStringValue = "Mirage-\(Self.fileTimestamp()).log"
                    panel.begin { response in
                        guard response == .OK, let url = panel.url else { return }
                        service.export(to: url)
                    }
                }
            }
            .padding(10)
        }
    }

    private static func fileTimestamp() -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        return formatter.string(from: Date())
    }
}

private struct DeveloperLogTextView: NSViewRepresentable {
    let text: String

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSScrollView()
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        scrollView.autohidesScrollers = true
        scrollView.drawsBackground = false
        let textView = NSTextView()
        textView.isEditable = false
        textView.isRichText = false
        textView.isSelectable = true
        textView.usesFindPanel = true
        textView.drawsBackground = false
        textView.isHorizontallyResizable = true
        textView.isVerticallyResizable = true
        textView.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        textView.textContainerInset = NSSize(width: 10, height: 10)
        textView.textContainer?.containerSize = textView.maxSize
        textView.textContainer?.widthTracksTextView = false
        textView.layoutManager?.allowsNonContiguousLayout = true
        scrollView.documentView = textView
        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        guard context.coordinator.text != text,
              let textView = scrollView.documentView as? NSTextView,
              let storage = textView.textStorage else { return }
        let atBottom = textView.bounds.height - scrollView.contentView.bounds.maxY < 40
        let previous = context.coordinator.text
        let attributes: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 11, weight: .regular),
            .foregroundColor: NSColor.labelColor
        ]
        storage.beginEditing()
        if text.hasPrefix(previous) {
            let suffix = (text as NSString).substring(from: (previous as NSString).length)
            storage.append(NSAttributedString(string: suffix, attributes: attributes))
        } else {
            storage.setAttributedString(NSAttributedString(string: text, attributes: attributes))
        }
        storage.endEditing()
        context.coordinator.text = text
        if atBottom || previous.isEmpty {
            textView.scrollRangeToVisible(NSRange(location: storage.length, length: 0))
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator {
        var text = ""
    }
}
