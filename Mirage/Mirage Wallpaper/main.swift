//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa

if WEConditionEvaluator.runWorkerIfRequested() { exit(0) }

let mirageOpenWindowNotification = Notification.Name("cn.laobamac.Mirage.openMainWindow")
let mirageLaunchAtLogin = ProcessInfo.processInfo.arguments.contains("--launch-at-login")

if let bundleID = Bundle.main.bundleIdentifier,
   NSWorkspace.shared.runningApplications.contains(where: {
       $0.bundleIdentifier == bundleID &&
       $0.processIdentifier != ProcessInfo.processInfo.processIdentifier
   }) {
    if !mirageLaunchAtLogin {
        DistributedNotificationCenter.default().postNotificationName(
            mirageOpenWindowNotification, object: nil, userInfo: nil, deliverImmediately: true)
    }
    exit(0)
}

NSApplication.shared.delegate = AppDelegate.shared
NSApplication.shared.run()
