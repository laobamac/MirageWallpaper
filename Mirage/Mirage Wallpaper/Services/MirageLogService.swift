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
    @Published private(set) var lastError: String?

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
    private var sessionFiles: [URL] = []
    private var sessionBytes = 0
    private var sessionPart = 0
    private let sessionID = UUID().uuidString
    private let maximumFileBytes: Int
    private let maximumSessionFiles: Int
    private let maximumHistoryBytes = 100 * 1024 * 1024
    private let maximumLineBytes = 64 * 1024
    private var streamBuffers: [String: Data] = [:]
    private var oversizedStreams: Set<String> = []
    private var stdoutPipe: Pipe?
    private var stderrPipe: Pipe?
    private var originalStdout: FileHandle?
    private var originalStderr: FileHandle?
    private var automaticSaveURL: URL?
    private var started = false
    private let logDirectory: URL?
    private let capturesStandardStreams: Bool
    private let automaticSaveDirectory: URL?

    init(logDirectory: URL? = nil, capturesStandardStreams: Bool = true,
         maximumFileBytes: Int = 5 * 1024 * 1024, maximumSessionFiles: Int = 4, automaticSaveDirectory: URL? = nil) {
        self.logDirectory = logDirectory
        self.automaticSaveDirectory = automaticSaveDirectory
        self.capturesStandardStreams = capturesStandardStreams
        self.maximumFileBytes = max(1024, maximumFileBytes)
        self.maximumSessionFiles = max(1, maximumSessionFiles)
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
            do {
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                removeExpiredSessions(in: directory)
                try openSessionPart(in: directory)
            } catch { report(error) }
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
        queue.async { [self] in appendOnQueue(message, source: source) }
    }

    private func appendOnQueue(_ message: String, source: String) {
        let sanitized = Self.redact(message)
        guard !sanitized.isEmpty else { return }
        let stamp = timestampFormatter.string(from: Date())
        let normalized = sanitized.hasSuffix("\n") ? sanitized : sanitized + "\n"
        var line = "[\(stamp)] [\(source)] \(normalized)"
        if line.utf8.count > min(maximumLineBytes, maximumFileBytes) {
            let bytes = line.utf8.prefix(min(maximumLineBytes, maximumFileBytes) - 40)
            line = String(decoding: bytes, as: UTF8.self) + "\n[log entry truncated]\n"
        }
        if let data = line.data(using: .utf8) {
            do {
                if sessionBytes + data.count > maximumFileBytes, let directory = sessionURL?.deletingLastPathComponent() {
                    try openSessionPart(in: directory)
                }
                try sessionHandle?.write(contentsOf: data)
                sessionBytes += data.count
            } catch { report(error) }
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

    func export(to url: URL, completion: @escaping (Result<URL, Error>) -> Void = { _ in }) {
        queue.async { [self] in
            let result = Result { try copySession(to: url); return url }
            if case .failure(let error) = result { report(error) }
            DispatchQueue.main.async { completion(result) }
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
            guard sessionURL != nil else { return nil }
            let destination: URL
            if let automaticSaveURL {
                destination = automaticSaveURL
            } else {
                let desktop = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]
                let directory = automaticSaveDirectory ?? desktop.appending(path: "MirageLogs", directoryHint: .isDirectory)
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                let candidate = directory.appending(path: "Mirage-Auto-\(sessionID).log")
                automaticSaveURL = candidate
                destination = candidate
            }
            try copySession(to: destination)
            removeExpiredSessions(in: destination.deletingLastPathComponent(), prefix: "Mirage-Auto-", protecting: [destination])
            return destination
        } catch {
            report(error)
            return nil
        }
    }

    private func report(_ error: Error) {
        let message = Self.redact(error.localizedDescription)
        DispatchQueue.main.async { [weak self] in self?.lastError = message }
    }

    private func openSessionPart(in directory: URL) throws {
        try sessionHandle?.synchronize()
        try sessionHandle?.close()
        sessionHandle = nil
        sessionPart += 1
        let url = directory.appending(path: "Session-\(sessionID)-\(sessionPart).log")
        guard FileManager.default.createFile(atPath: url.path, contents: nil,
                                             attributes: [.posixPermissions: 0o600]) else {
            throw CocoaError(.fileWriteUnknown)
        }
        sessionHandle = try FileHandle(forWritingTo: url)
        sessionURL = url
        sessionBytes = 0
        sessionFiles.append(url)
        while sessionFiles.count > maximumSessionFiles {
            try FileManager.default.removeItem(at: sessionFiles.removeFirst())
        }
        removeExpiredSessions(in: directory)
    }

    /// Copy bounded chunks to a sibling temporary file, leaving an existing
    /// export intact if reading or writing fails.
    private func copySession(to destination: URL) throws {
        guard !sessionFiles.isEmpty,
              !sessionFiles.contains(where: { $0.resolvingSymlinksInPath() == destination.resolvingSymlinksInPath() }) else {
            throw CocoaError(.fileWriteInvalidFileName)
        }
        try sessionHandle?.synchronize()
        let temporary = destination.deletingLastPathComponent().appending(path: ".MirageLog-\(UUID().uuidString)")
        guard FileManager.default.createFile(atPath: temporary.path, contents: nil,
                                             attributes: [.posixPermissions: 0o600]) else {
            throw CocoaError(.fileWriteUnknown)
        }
        defer { try? FileManager.default.removeItem(at: temporary) }
        let output = try FileHandle(forWritingTo: temporary)
        defer { try? output.close() }
        for file in sessionFiles {
            let input = try FileHandle(forReadingFrom: file)
            defer { try? input.close() }
            while let data = try input.read(upToCount: 64 * 1024), !data.isEmpty {
                try output.write(contentsOf: data)
            }
        }
        try output.synchronize()
        try output.close()
        guard rename(temporary.path, destination.path) == 0 else {
            throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
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
                self?.consume(data, source: "stdout", original: nil)
                return
            }
            self?.consume(data, source: "stdout", original: self?.originalStdout)
        }
        stderrPipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else {
                handle.readabilityHandler = nil
                self?.consume(data, source: "stderr", original: nil)
                return
            }
            self?.consume(data, source: "stderr", original: self?.originalStderr)
        }
    }

    private func removeExpiredSessions(in directory: URL, prefix: String = "Session-", protecting: [URL]? = nil) {
        let protected = protecting ?? sessionFiles
        guard let expiration = Calendar.current.date(byAdding: .day, value: -7, to: Date()),
              let urls = try? FileManager.default.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: [.contentModificationDateKey],
                options: [.skipsHiddenFiles]
              ) else { return }
        let files = urls.filter {
            guard $0.lastPathComponent.hasPrefix(prefix), $0.pathExtension == "log" else { return false }
            return prefix != "Mirage-Auto-" || UUID(uuidString: String($0.deletingPathExtension().lastPathComponent.dropFirst(prefix.count))) != nil
        }
            .sorted { ((try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast)
                < ((try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast) }
        var total = files.reduce(0) { $0 + ((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
        for url in files where !protected.contains(url) {
            let values = try? url.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
            guard (values?.contentModificationDate ?? .distantPast) < expiration
                    || total > maximumHistoryBytes - maximumFileBytes else { continue }
            do {
                try FileManager.default.removeItem(at: url)
                total -= values?.fileSize ?? 0
            } catch { report(error) }
        }
    }

    func consume(_ data: Data, source: String, original: FileHandle? = nil) {
        try? original?.write(contentsOf: data)
        queue.async { [self] in
            var buffer = streamBuffers[source] ?? Data()
            for byte in data {
                if byte == 10 {
                    if oversizedStreams.remove(source) != nil {
                        appendOnQueue("[oversized log line omitted]", source: source)
                    } else {
                        appendOnQueue(String(decoding: buffer, as: UTF8.self), source: source)
                    }
                    buffer.removeAll(keepingCapacity: true)
                } else if !oversizedStreams.contains(source) {
                    buffer.append(byte)
                    if buffer.count > maximumLineBytes {
                        buffer.removeAll(keepingCapacity: true)
                        oversizedStreams.insert(source)
                    }
                }
            }
            if data.isEmpty, !buffer.isEmpty, !oversizedStreams.contains(source) {
                appendOnQueue(String(decoding: buffer, as: UTF8.self), source: source)
                buffer.removeAll()
            }
            streamBuffers[source] = buffer
        }
    }

    private static let redactions: [(NSRegularExpression, String)] = {
        let replacements = [
            (#"(?i)(\bauthorization\s*[=:]\s*)(?:bearer|basic)\s+[^\s,;]+"#, "$1<redacted>"),
            (#"(?im)(\bcookie\s*[=:]\s*)[^\r\n]+"#, "$1<redacted>"),
            (#"(?i)(\bbearer\s+)[A-Za-z0-9._~+/=\-]+"#, "$1<redacted>"),
            (#"(?i)((?:\"|')?(?:key|api[_-]?key|steam[_-]?api[_-]?key|token|access[_-]?token|refresh[_-]?token|password|passwd|pin|secret|private[_-]?key|aes[_-]?key|session[_-]?key|steamguard|guard[_-]?code|authorization|cookie)(?:\"|')?\s*[=:]\s*)(?:\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[^\s&,}]+)"#, "$1<redacted>"),
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
                if let error = service.lastError {
                    Text(error).font(.caption).foregroundStyle(.red).lineLimit(2)
                }
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
