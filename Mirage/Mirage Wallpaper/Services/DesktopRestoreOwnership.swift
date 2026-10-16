import Foundation

/// A recent write may still read back the previous picture. That short-lived
/// ambiguity must not grant ownership for the rest of the app's lifetime.
enum DesktopRestoreOwnership {
    static func shouldRestore(current: URL?, installed: URL?, previous: URL?,
                              recentWriteAge: TimeInterval?, recorded: Bool,
                              isCurrentGenerated: Bool) -> Bool {
        if let current {
            let current = current.resolvingSymlinksInPath()
            // Older captures and lock-screen fallback files are still owned
            // by Mirage and must not be pruned while the desktop points at them.
            if isCurrentGenerated { return true }
            if let installed, current == installed.resolvingSymlinksInPath() { return true }
            if installed == nil { return false }
            if let age = recentWriteAge, (0...2).contains(age),
               let previous, current == previous.resolvingSymlinksInPath() {
                return true
            }
            return false
        }
        // An unavailable readback provides no evidence of an external change.
        return installed != nil || recorded
    }
}
