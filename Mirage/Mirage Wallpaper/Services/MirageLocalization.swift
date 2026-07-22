//
//  MirageLocalization.swift
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

/// Centralizes Mirage's application language.  SwiftUI receives `locale` through
/// the environment, while AppKit and service-layer messages use `L(...)`.
final class MirageLocalization: ObservableObject {
    static let shared = MirageLocalization()

    static let didChangeNotification = Notification.Name("MirageLocalizationDidChange")

    @Published private(set) var locale = Locale(identifier: "en")
    private(set) var language: GSLocalization = .followSystem

    private init() {}

    func apply(_ requestedLanguage: GSLocalization) {
        let resolved = Self.resolve(requestedLanguage)
        let changed = language != requestedLanguage || locale.identifier != resolved.identifier
        language = requestedLanguage
        locale = resolved
        guard changed else { return }
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }

    func string(_ key: String, arguments: [CVarArg] = []) -> String {
        let format = activeBundle.localizedString(forKey: key, value: key, table: "Localizable")
        guard !arguments.isEmpty else { return format }
        return String(format: format, locale: locale, arguments: arguments)
    }

    private var activeBundle: Bundle {
        let resource: String
        switch resolvedLanguage {
        case .english: resource = "en"
        case .simplifiedChinese: resource = "zh-Hans"
        case .traditionalChinese: resource = "zh-Hant"
        }
        guard let path = Bundle.main.path(forResource: resource, ofType: "lproj"),
              let bundle = Bundle(path: path) else { return .main }
        return bundle
    }

    private enum ResolvedLanguage {
        case english, simplifiedChinese, traditionalChinese
    }

    private var resolvedLanguage: ResolvedLanguage {
        switch language {
        case .en_US: return .english
        case .zh_CN: return .simplifiedChinese
        case .zh_TW: return .traditionalChinese
        case .followSystem:
            let preferred = Locale.preferredLanguages.first?.lowercased() ?? "en"
            if preferred.hasPrefix("zh-hant") || preferred.hasPrefix("zh-tw") || preferred.hasPrefix("zh-hk") {
                return .traditionalChinese
            }
            return preferred.hasPrefix("zh") ? .simplifiedChinese : .english
        }
    }

    private static func resolve(_ requestedLanguage: GSLocalization) -> Locale {
        switch requestedLanguage {
        case .en_US: return Locale(identifier: "en")
        case .zh_CN: return Locale(identifier: "zh-Hans")
        case .zh_TW: return Locale(identifier: "zh-Hant")
        case .followSystem:
            let preferred = Locale.preferredLanguages.first?.lowercased() ?? "en"
            if preferred.hasPrefix("zh-hant") || preferred.hasPrefix("zh-tw") || preferred.hasPrefix("zh-hk") {
                return Locale(identifier: "zh-Hant")
            }
            return preferred.hasPrefix("zh") ? Locale(identifier: "zh-Hans") : Locale(identifier: "en")
        }
    }
}

@inline(__always)
func L(_ key: String, _ arguments: CVarArg...) -> String {
    MirageLocalization.shared.string(key, arguments: arguments)
}
