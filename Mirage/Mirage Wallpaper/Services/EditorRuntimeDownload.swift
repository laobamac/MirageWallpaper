// Mirage Wallpaper
// Copyright © 2026 王孝慈. All rights reserved.

import Foundation
import Darwin

enum EditorRuntimeDownload {
    enum Phase: Sendable { case downloading, verifying, unpacking }
    struct Progress: Sendable {
        let phase: Phase
        let received: Int64
        let total: Int64
        var bytesPerSecond: Double = 0
    }

    static func prepare(catalog: EditorCatalog, configuration: URLSessionConfiguration = .ephemeral,
                        progress: @escaping @Sendable (Progress) -> Void) async throws {
        guard let archive = catalog.archive else { return }
        try EditorInstallation.validateCompatibility(catalog)
        try EditorInstallation.validateRoot()
        let fm = FileManager.default
        let destination = EditorInstallation.runtimeCache.appendingPathComponent(archive.sha256)
        let payload = destination.appendingPathComponent("EditorSupport")
        for directory in [EditorInstallation.runtimeDownloads, EditorInstallation.runtimeCache] {
            try fm.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        }
        guard (try? destination.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true else {
            throw EditorServiceError(code: "EDITOR_FILES")
        }
        if fm.fileExists(atPath: payload.path) {
            do {
                progress(Progress(phase: .verifying, received: archive.bytes, total: archive.bytes))
                try await work { try EditorInstallation.verifySupport(payload, catalog: catalog) }
                return
            } catch is CancellationError { throw CancellationError() }
            catch { try fm.removeItem(at: destination) }
        }
        let complete = EditorInstallation.runtimeDownloads.appendingPathComponent(archive.sha256 + ".tar.gz")
        let partial = complete.appendingPathExtension("partial")
        for file in [complete, partial] {
            guard (try? file.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) != true else {
                throw EditorServiceError(code: "EDITOR_FILES")
            }
        }
        var cached = false
        if fm.fileExists(atPath: complete.path) {
            cached = try await work { try EditorInstallation.hash(complete) == archive.sha256 }
            if !cached { try fm.removeItem(at: complete) }
        }
        if !cached {
            let transfer = try EditorArchiveTransfer(archive: archive, partial: partial, configuration: configuration) {
                progress(Progress(phase: .downloading, received: $0, total: archive.bytes, bytesPerSecond: $1))
            }
            try await transfer.download()
            progress(Progress(phase: .verifying, received: archive.bytes, total: archive.bytes))
            guard try await work({ try EditorInstallation.hash(partial) == archive.sha256 }) else {
                try fm.removeItem(at: partial)
                throw EditorServiceError(code: "EDITOR_FILES")
            }
            try Task.checkCancellation()
            try fm.moveItem(at: partial, to: complete)
        }
        progress(Progress(phase: .unpacking, received: archive.bytes, total: archive.bytes))
        try await work {
            let fm = FileManager.default
            let staging = EditorInstallation.runtimeCache.appendingPathComponent("preparing-" + UUID().uuidString)
            try fm.createDirectory(at: staging, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            defer { try? fm.removeItem(at: staging) }
            // The archive's exact bytes are pinned by the signed app's manifest.
            // Extract into a fresh private directory, then verify every catalog entry.
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
            process.arguments = ["-xzf", complete.path, "--no-same-owner", "-C", staging.path]
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            try process.run()
            while process.isRunning {
                if Task.isCancelled {
                    process.terminate()
                    process.waitUntilExit()
                    throw CancellationError()
                }
                Thread.sleep(forTimeInterval: 0.05)
            }
            guard process.terminationStatus == 0 else { throw EditorServiceError(code: "EDITOR_FILES") }
            try EditorInstallation.verifySupport(staging.appendingPathComponent("EditorSupport"), catalog: catalog)
            try Task.checkCancellation()
            if fm.fileExists(atPath: destination.path) { try fm.removeItem(at: destination) }
            try fm.moveItem(at: staging, to: destination)
        }
    }

    private static func work<T: Sendable>(_ body: @escaping @Sendable () throws -> T) async throws -> T {
        let task = Task.detached(priority: .utility, operation: body)
        return try await withTaskCancellationHandler { try await task.value } onCancel: { task.cancel() }
    }
}

/// Writes response chunks directly to disk and resumes only a validated byte range.
/// URLSession delegate callbacks use one serial queue; cancellation uses the lock.
final class EditorArchiveTransfer: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    private let archive: EditorRuntimeArchive
    private let file: FileHandle
    private let configuration: URLSessionConfiguration
    private let progress: @Sendable (Int64, Double) -> Void
    private let lock = NSLock()
    private var cancelled = false
    private var requestTask: URLSessionDataTask?
    private var session: URLSession?
    private var continuation: CheckedContinuation<Void, Error>?
    private var failure: Error?
    private var received: Int64
    private var initial: Int64
    private var started = Date()
    private var lastProgress = Date.distantPast

    init(archive: EditorRuntimeArchive, partial: URL, configuration: URLSessionConfiguration,
         progress: @escaping @Sendable (Int64, Double) -> Void) throws {
        self.archive = archive
        self.configuration = configuration
        self.progress = progress
        let descriptor = Darwin.open(partial.path, O_RDWR | O_CREAT | O_NOFOLLOW, 0o600)
        guard descriptor >= 0 else { throw EditorServiceError(code: "EDITOR_FILES") }
        file = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        received = Int64(try file.seekToEnd())
        if received > archive.bytes { try file.truncate(atOffset: 0); try file.seek(toOffset: 0); received = 0 }
        initial = received
        super.init()
    }

    func download() async throws {
        try Task.checkCancellation()
        if received == archive.bytes {
            try file.close()
            progress(received, 0)
            return
        }
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                lock.lock()
                if cancelled {
                    lock.unlock()
                    try? file.close()
                    continuation.resume(throwing: CancellationError())
                    return
                }
                self.continuation = continuation
                let queue = OperationQueue()
                queue.maxConcurrentOperationCount = 1
                configuration.urlCache = nil
                configuration.httpCookieStorage = nil
                configuration.urlCredentialStorage = nil
                configuration.timeoutIntervalForRequest = 60
                session = URLSession(configuration: configuration, delegate: self, delegateQueue: queue)
                var request = URLRequest(url: archive.url, cachePolicy: .reloadIgnoringLocalCacheData)
                request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
                if received > 0 { request.setValue("bytes=\(received)-", forHTTPHeaderField: "Range") }
                requestTask = session!.dataTask(with: request)
                requestTask!.resume()
                lock.unlock()
            }
        } onCancel: {
            self.lock.lock()
            self.cancelled = true
            self.requestTask?.cancel()
            self.lock.unlock()
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        guard request.url?.scheme == "https" else {
            failure = EditorServiceError(code: "EDITOR_RUNTIME_DOWNLOAD")
            completionHandler(nil)
            return
        }
        completionHandler(request)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        do {
            guard let response = response as? HTTPURLResponse else { throw EditorServiceError(code: "EDITOR_RUNTIME_DOWNLOAD") }
            if response.statusCode == 206 {
                let expected = "bytes \(received)-\(archive.bytes - 1)/\(archive.bytes)"
                guard response.value(forHTTPHeaderField: "Content-Range") == expected else {
                    throw EditorServiceError(code: "EDITOR_RUNTIME_DOWNLOAD")
                }
            } else if response.statusCode == 200 {
                // Some CDNs ignore Range. Replace the partial file with their full response.
                try file.truncate(atOffset: 0)
                try file.seek(toOffset: 0)
                received = 0
                initial = 0
            } else { throw EditorServiceError(code: "EDITOR_RUNTIME_DOWNLOAD") }
            if response.expectedContentLength >= 0 && response.expectedContentLength != archive.bytes - received {
                throw EditorServiceError(code: "EDITOR_RUNTIME_DOWNLOAD")
            }
            started = Date()
            progress(received, 0)
            completionHandler(.allow)
        } catch {
            failure = error
            completionHandler(.cancel)
            dataTask.cancel()
        }
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard failure == nil else { return }
        do {
            guard Int64(data.count) <= archive.bytes - received else { throw EditorServiceError(code: "EDITOR_FILES") }
            try file.write(contentsOf: data)
            received += Int64(data.count)
            let now = Date()
            if now.timeIntervalSince(lastProgress) >= 0.15 || received == archive.bytes {
                progress(received, Double(received - initial) / max(0.001, now.timeIntervalSince(started)))
                lastProgress = now
            }
        } catch {
            failure = error
            dataTask.cancel()
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        try? file.close()
        lock.lock()
        let wasCancelled = cancelled
        let continuation = self.continuation
        self.continuation = nil
        requestTask = nil
        self.session = nil
        lock.unlock()
        session.finishTasksAndInvalidate()
        if wasCancelled { continuation?.resume(throwing: CancellationError()) }
        else if let error = failure ?? error {
            continuation?.resume(throwing: error is EditorServiceError ? error : EditorServiceError(code: "EDITOR_RUNTIME_DOWNLOAD"))
        }
        else if received != archive.bytes { continuation?.resume(throwing: EditorServiceError(code: "EDITOR_RUNTIME_DOWNLOAD")) }
        else { continuation?.resume() }
    }
}
