import Foundation

@main
struct DesktopRestoreRegression {
    static func main() {
        let original = URL(fileURLWithPath: "/tmp/original.jpg")
        let installed = URL(fileURLWithPath: "/tmp/Mirage/current.heic")
        let external = URL(fileURLWithPath: "/tmp/new-choice.jpg")
        func check(_ expected: Bool, _ name: String, current: URL?,
                   installed: URL? = installed, previous: URL? = original,
                   age: TimeInterval? = nil, recorded: Bool = true, generated: Bool = false) {
            let result = DesktopRestoreOwnership.shouldRestore(
                current: current, installed: installed, previous: previous,
                recentWriteAge: age, recorded: recorded, isCurrentGenerated: generated)
            guard result == expected else { fatalError(name) }
            print("PASS: \(name)")
        }
        check(true, "owned picture restores", current: installed, generated: true)
        check(true, "older owned capture still restores safely", current: URL(fileURLWithPath: "/tmp/Mirage/older.heic"), generated: true)
        check(false, "external picture survives quit", current: external)
        check(false, "external picture survives even during readback grace", current: external, age: 0.1)
        check(true, "immediate quit restores while readback still reports previous picture", current: original, age: 0.1)
        check(false, "old picture is an external choice after grace expires", current: original, age: 2.1)
        check(false, "acknowledged install no longer permits stale previous picture", current: original)
        check(true, "crash recovery restores a generated picture", current: installed, installed: nil, generated: true)
        check(false, "crash recovery preserves an external picture", current: external, installed: nil)
        check(false, "unowned screen is untouched", current: external, installed: nil, recorded: false)
        check(true, "temporarily unavailable readback retains recovery", current: nil)
        check(false, "unavailable readback with no ownership does nothing", current: nil, installed: nil, recorded: false)
    }
}
