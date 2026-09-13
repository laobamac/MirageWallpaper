//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

enum DynamicLockScreenMode: String {
    case extensionMode = "extension"
    case screenSaver = "screenSaver"
}

enum DynamicLockScreenModeStore {
    private static let key = "Mirage.DynamicLockScreen.Mode"

    static var active: DynamicLockScreenMode? {
        guard let raw = UserDefaults.standard.string(forKey: key) else { return nil }
        return DynamicLockScreenMode(rawValue: raw)
    }

    static func activate(_ mode: DynamicLockScreenMode) {
        UserDefaults.standard.set(mode.rawValue, forKey: key)
        UserDefaults.standard.synchronize()
    }

    static func deactivate(_ mode: DynamicLockScreenMode) {
        guard active == mode else { return }
        UserDefaults.standard.removeObject(forKey: key)
        UserDefaults.standard.synchronize()
    }
}
