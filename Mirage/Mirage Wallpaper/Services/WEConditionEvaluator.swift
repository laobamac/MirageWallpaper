//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import JavaScriptCore
import Darwin

final class WEConditionEvaluator {
    static let workerArgument = "--mirage-condition-worker"

    private struct Request {
        let identity: String
        let conditions: [String]
        let values: [String: Any]
    }

    private let worker: Worker
    private let pipeline: LatestValueWorker<Request, [String: Bool]>

    init(executableURL: URL? = Bundle.main.executableURL,
         evaluationTimeout: TimeInterval = 0.25,
         startupTimeout: TimeInterval = 2) {
        let worker = Worker(executableURL: executableURL,
                            evaluationTimeout: evaluationTimeout,
                            startupTimeout: startupTimeout)
        self.worker = worker
        pipeline = LatestValueWorker(label: "cn.laobamac.Mirage.conditions") {
            worker.evaluate($0)
        }
    }

    func evaluate(identity: String, conditions: [String], values: [String: Any],
                  completion: @escaping ([String: Bool]) -> Void) {
        pipeline.submit(Request(identity: identity, conditions: conditions, values: values),
                        completion: completion)
    }

    func cancel() {
        pipeline.cancel()
        worker.cancel()
    }

    deinit {
        cancel()
    }

    static func runWorkerIfRequested() -> Bool {
        guard CommandLine.arguments.contains(workerArgument) else { return false }
        let parent = DispatchSource.makeProcessSource(identifier: getppid(), eventMask: .exit,
                                                      queue: .global(qos: .utility))
        parent.setEventHandler { _exit(0) }
        parent.resume()
        defer { parent.cancel() }
        let machine = JSVirtualMachine()
        while let line = readLine() {
            autoreleasepool {
                guard let data = line.data(using: .utf8), data.count <= 8 * 1024 * 1024,
                      let request = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                      let id = request["id"] as? String,
                      let conditions = request["conditions"] as? [String],
                      let values = request["values"] as? [String: Any] else { return }
                let context = JSContext(virtualMachine: machine)
                context?.exceptionHandler = { _, _ in }
                for (key, value) in values {
                    context?.setObject(value, forKeyedSubscript: key as NSString)
                }
                var verdicts: [String: Bool] = [:]
                for condition in conditions {
                    context?.exception = nil
                    let result = context?.evaluateScript(condition)
                    if context?.exception != nil {
                        verdicts[condition] = true
                    } else {
                        verdicts[condition] = verdict(result)
                    }
                }
                guard var response = try? JSONSerialization.data(withJSONObject: [
                    "id": id, "verdicts": verdicts
                ]) else { return }
                response.append(0x0A)
                try? FileHandle.standardOutput.write(contentsOf: response)
            }
        }
        return true
    }

    private static func verdict(_ result: JSValue?) -> Bool {
        guard let result else { return true }
        if result.isBoolean { return result.toBool() }
        if result.isNumber { return result.toDouble() != 0 }
        if result.isNull || result.isUndefined { return true }
        return result.toBool()
    }

    private final class Worker {
        private let executableURL: URL?
        private let evaluationTimeout: TimeInterval
        private let startupTimeout: TimeInterval
        private let lock = NSLock()
        private var process: Process?
        private var input: Pipe?
        private var output: Pipe?
        private var cancellation: UInt64 = 0
        private var identity: String?
        private var failures = 0

        init(executableURL: URL?, evaluationTimeout: TimeInterval, startupTimeout: TimeInterval) {
            self.executableURL = executableURL
            self.evaluationTimeout = evaluationTimeout
            self.startupTimeout = startupTimeout
        }

        func evaluate(_ request: Request) -> [String: Bool] {
            if identity != request.identity {
                cancel()
                identity = request.identity
                failures = 0
            }
            guard failures < 3, !request.conditions.isEmpty else { return [:] }
            let id = UUID().uuidString
            guard var data = try? JSONSerialization.data(withJSONObject: [
                "id": id, "conditions": request.conditions, "values": request.values
            ]), data.count <= 8 * 1024 * 1024 else { return [:] }
            data.append(0x0A)
            guard let connection = connection() else { return [:] }
            let reply = Reply(id: id)
            let reader = connection.output.fileHandleForReading
            reader.readabilityHandler = { handle in
                let chunk = handle.availableData
                reply.consume(chunk)
                if chunk.isEmpty { handle.readabilityHandler = nil }
            }
            let timeout = connection.started ? startupTimeout : evaluationTimeout
            let deadline = DispatchTime.now() + timeout
            let timeoutWork = DispatchWorkItem { [weak self] in
                guard reply.markTimedOut() else { return }
                self?.cancel(matching: connection.process)
                reply.ready.signal()
            }
            DispatchQueue.global(qos: .utility).asyncAfter(deadline: deadline, execute: timeoutWork)
            defer {
                timeoutWork.cancel()
                reader.readabilityHandler = nil
            }
            do {
                try connection.input.fileHandleForWriting.write(contentsOf: data)
            } catch {
                if reply.timedOut || isCurrent(connection.process) { failures += 1 }
                cancel(matching: connection.process)
                return [:]
            }
            guard reply.ready.wait(timeout: deadline) == .success,
                  let result = reply.result else {
                if reply.timedOut || isCurrent(connection.process) { failures += 1 }
                cancel(matching: connection.process)
                return [:]
            }
            failures = 0
            return result
        }

        private func connection() -> (process: Process, input: Pipe, output: Pipe, started: Bool)? {
            lock.lock()
            let generation = cancellation
            if let process, process.isRunning, let input, let output {
                lock.unlock()
                return (process, input, output, false)
            }
            lock.unlock()
            guard let executableURL else { return nil }
            let next = Process()
            let stdin = Pipe()
            let stdout = Pipe()
            next.executableURL = executableURL
            next.arguments = [WEConditionEvaluator.workerArgument]
            next.standardInput = stdin
            next.standardOutput = stdout
            next.standardError = FileHandle.nullDevice
            _ = fcntl(stdin.fileHandleForWriting.fileDescriptor, F_SETNOSIGPIPE, 1)
            do {
                try next.run()
            } catch {
                return nil
            }
            lock.lock()
            guard cancellation == generation else {
                lock.unlock()
                if next.isRunning { kill(next.processIdentifier, SIGKILL) }
                return nil
            }
            process = next
            input = stdin
            output = stdout
            lock.unlock()
            return (next, stdin, stdout, true)
        }

        private func isCurrent(_ candidate: Process) -> Bool {
            lock.lock()
            defer { lock.unlock() }
            return process === candidate
        }

        func cancel(matching candidate: Process? = nil) {
            lock.lock()
            if let candidate, process !== candidate {
                lock.unlock()
                return
            }
            cancellation &+= 1
            let previous = process
            process = nil
            input = nil
            output = nil
            lock.unlock()
            if let previous, previous.isRunning {
                kill(previous.processIdentifier, SIGKILL)
            }
        }

        deinit { cancel() }
    }

    private final class Reply {
        let ready = DispatchSemaphore(value: 0)
        private let id: String
        private let lock = NSLock()
        private var buffer = Data()
        private var finished = false
        private var expired = false
        private var value: [String: Bool]?

        init(id: String) { self.id = id }

        var result: [String: Bool]? {
            lock.lock()
            defer { lock.unlock() }
            return value
        }

        var timedOut: Bool {
            lock.lock()
            defer { lock.unlock() }
            return expired
        }

        func markTimedOut() -> Bool {
            lock.lock()
            defer { lock.unlock() }
            guard !finished else { return false }
            finished = true
            expired = true
            return true
        }

        func consume(_ data: Data) {
            lock.lock()
            defer { lock.unlock() }
            guard !finished else { return }
            buffer.append(data)
            if let newline = buffer.firstIndex(of: 0x0A),
               let response = try? JSONSerialization.jsonObject(with: Data(buffer[..<newline])) as? [String: Any],
               response["id"] as? String == id {
                value = response["verdicts"] as? [String: Bool]
                finished = true
            } else if data.isEmpty || buffer.count > 8 * 1024 * 1024 {
                finished = true
            }
            if finished { ready.signal() }
        }
    }
}
