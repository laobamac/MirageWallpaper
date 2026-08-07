//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import JavaScriptCore

// Evaluates Wallpaper Engine property/option `condition` expressions (JS, e.g.
// "clock.value == true" or "a.value == 1 && [1,2].includes(b.value)"). Empty or
// failing expressions resolve to visible so a stray condition never hides a row.
//
// The expressions are copied verbatim out of a third-party project.json and are
// therefore untrusted. `evaluate` is called from a SwiftUI `body`, so a
// condition of "while(1){}" would otherwise wedge the main thread for good.
//
// JavaScriptCore can bound script execution with
// JSContextGroupSetExecutionTimeLimit, but that function is declared in the
// private JSContextRefPrivate.h header: it is not part of the JavaScriptCore
// module shipped in the public macOS SDK, and JSVirtualMachine exposes no
// JSContextGroupRef either, so it cannot be reached from Swift here without
// private-API tricks. Every expression therefore runs on a dedicated serial
// queue while the caller waits with a short timeout.
//
// Caveat of that approach: a script that never returns keeps spinning on that
// queue forever, because JavaScriptCore offers no public way to interrupt it.
// A run of consecutive timeouts therefore retires the evaluator for good — its
// JSContext and queue are released (the runaway job still owns them, which is
// why a fresh context would be required and why we simply stop evaluating
// instead), the cache is cleared, and every later condition resolves to visible.
// The price is one spinning thread leaked per malicious wallpaper; the UI stays
// responsive.
final class WEConditionEvaluator {

    // Orders of magnitude above any legitimate condition (a comparison or two).
    private static let evaluationTimeout: DispatchTimeInterval = .milliseconds(100)

    // The very first evaluation also pays for JavaScriptCore warm-up plus a
    // queue hop, which on a loaded machine can exceed the steady-state budget
    // through no fault of the expression. Retiring is permanent, so that one
    // gets room to breathe.
    private static let firstEvaluationTimeout: DispatchTimeInterval = .milliseconds(500)

    // Retire only after a *run* of timeouts. A genuinely wedged queue never
    // drains, so a runaway condition trips this almost immediately, while a
    // single unlucky sample under load cannot permanently disable the editor.
    private static let maxConsecutiveTimeouts = 3

    private var consecutiveTimeouts = 0
    private var hasEvaluatedOnce = false

    // nil once a condition has timed out: evaluation is disabled from then on.
    private var runtime: Runtime? = Runtime()

    // Condition string -> last verdict. Cleared whenever the values behind the
    // expressions change, so a SwiftUI body recomputation does not re-enter
    // JavaScriptCore once per row (and per combo option) on every pass.
    private var cache: [String: Bool] = [:]

    // `cache` and `runtime` are only ever touched by the caller (the main thread
    // in practice); the worker queue only ever touches the JSContext. The lock
    // keeps that split safe even if a condition is evaluated off the main thread.
    private let lock = NSLock()

    func updateContext(properties: [String: WEProjectProperty],
                       overrides: [String: WEPropertyValue]) {
        lock.lock()
        cache.removeAll()
        let runtime = self.runtime
        lock.unlock()
        guard let runtime, let context = runtime.context else { return }

        var values: [String: Any] = [:]
        values.reserveCapacity(properties.count)
        for (key, prop) in properties {
            let raw = overrides[key] ?? prop.value
            values[key] = ["value": jsValue(for: raw, type: prop.propertyType)]
        }
        // Never wait here: if a runaway script already owns the queue this must
        // not block the UI. The queue is serial, so a later evaluate() still
        // observes these values.
        runtime.queue.async {
            for (key, value) in values {
                context.setObject(value, forKeyedSubscript: key as NSString)
            }
        }
    }

    func evaluate(_ condition: String?) -> Bool {
        guard let condition, !condition.trimmingCharacters(in: .whitespaces).isEmpty else {
            return true
        }
        lock.lock()
        if let cached = cache[condition] {
            lock.unlock()
            return cached
        }
        let runtime = self.runtime
        let timeout = hasEvaluatedOnce ? Self.evaluationTimeout : Self.firstEvaluationTimeout
        lock.unlock()
        // Retired evaluator: everything stays visible.
        guard let runtime, let context = runtime.context else { return true }

        let box = ResultBox()
        let semaphore = DispatchSemaphore(value: 0)
        runtime.queue.async {
            box.value = WEConditionEvaluator.verdict(of: context.evaluateScript(condition))
            semaphore.signal()
        }
        guard semaphore.wait(timeout: .now() + timeout) == .success else {
            lock.lock()
            hasEvaluatedOnce = true
            consecutiveTimeouts += 1
            let exhausted = consecutiveTimeouts >= Self.maxConsecutiveTimeouts
            lock.unlock()
            if exhausted { retire() }
            return true
        }
        // The semaphore orders the worker's write before this read.
        let result = box.value
        lock.lock()
        hasEvaluatedOnce = true
        consecutiveTimeouts = 0
        cache[condition] = result
        lock.unlock()
        return result
    }

    // Drops the wedged context and its queue, and stops evaluating for good.
    private func retire() {
        lock.lock()
        let wasActive = runtime != nil
        runtime = nil
        cache.removeAll()
        lock.unlock()
        if wasActive {
            NSLog("[Mirage] 条件表达式执行超时，已停用该壁纸的条件求值")
        }
    }

    private static func verdict(of result: JSValue?) -> Bool {
        guard let result else { return true }
        if result.isBoolean { return result.toBool() }
        if result.isNumber { return result.toDouble() != 0 }
        if result.isNull || result.isUndefined { return true }
        return result.toBool()
    }

    private func jsValue(for value: WEPropertyValue, type: WEPropertyType) -> Any {
        switch value {
        case .bool(let b): return b
        case .number(let d): return d
        case .string(let s):
            if type == .bool { return (s as NSString).boolValue }
            if let i = Int(s) { return i }
            if let d = Double(s) { return d }
            if s == "true" { return true }
            if s == "false" { return false }
            return s
        }
    }

    // Ties the JSContext to the one queue that is allowed to touch it.
    private final class Runtime {
        let queue = DispatchQueue(label: "com.mirage.wallpaper.condition-evaluator",
                                  qos: .userInitiated)
        let context = JSContext()

        init() { context?.exceptionHandler = { _, _ in } }
    }

    // One box per evaluation, so a timed-out job can never overwrite the result
    // a later evaluation is waiting for.
    private final class ResultBox {
        var value = true
    }
}
