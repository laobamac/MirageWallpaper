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
final class WEConditionEvaluator {
    private let context = JSContext()

    init() { context?.exceptionHandler = { _, _ in } }

    func updateContext(properties: [String: WEProjectProperty],
                       overrides: [String: WEPropertyValue]) {
        guard let context else { return }
        for (key, prop) in properties {
            let raw = overrides[key] ?? prop.value
            context.setObject(["value": jsValue(for: raw, type: prop.propertyType)],
                              forKeyedSubscript: key as NSString)
        }
    }

    func evaluate(_ condition: String?) -> Bool {
        guard let condition, !condition.trimmingCharacters(in: .whitespaces).isEmpty else {
            return true
        }
        guard let context, let result = context.evaluateScript(condition) else { return true }
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
}
