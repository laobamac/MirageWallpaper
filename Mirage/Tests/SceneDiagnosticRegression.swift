private let sceneDiagnosticRegressionCopyright = "Copyright © 2026 王孝慈. All rights reserved."

import Foundation

func L(_ key: String) -> String { key }

@main
struct SceneDiagnosticRegression {
    @MainActor
    static func main() async throws {
        var checks = 0
        func check(_ value: @autoclosure () -> Bool, _ name: String) {
            precondition(value(), name)
            checks += 1
        }
        let defaults = SceneDiagnosticCase.plan(fastMath: "0", metalFX: false)
        check(defaults.map(\.id) == ["A", "R", "B", "C", "D"], "case order")
        check(defaults[3].status == "skipped", "skip redundant fast math")
        check(defaults[4].status == "skipped", "skip redundant MetalFX")
        let automatic = SceneDiagnosticCase.plan(fastMath: "default", metalFX: false)
        check(automatic[0].fastMath == "default" && automatic[3].status == "pending", "preserve runtime defaults and test fast math independently")
        let enabled = SceneDiagnosticCase.plan(fastMath: "2", metalFX: true)
        check(enabled.allSatisfy { $0.status == "pending" }, "enabled comparisons")
        check(enabled[2].fastMath == enabled[1].fastMath && enabled[2].metalFX == enabled[1].metalFX, "matched patch controls")
        check(enabled[3].library == enabled[0].library && enabled[3].metalFX == enabled[0].metalFX, "isolated fast math")
        check(enabled[4].library == enabled[0].library && enabled[4].fastMath == enabled[0].fastMath, "isolated MetalFX")
        var results = defaults
        for index in 0...2 { results[index].status = "completed"; results[index].observation = index == 2 ? "normal" : "green" }
        check(SceneDiagnosticCase.conclusion(results) == "controlled_patch_supported_not_yet_release_verified", "patch attribution")
        results[1].observation = "normal"
        check(SceneDiagnosticCase.conclusion(results) == "dependency_change_requires_further_analysis", "do not attribute unrelated dependency changes")
        results[0].observation = "normal"
        check(SceneDiagnosticCase.conclusion(results) == "baseline_not_reproduced_or_unconfirmed", "require baseline reproduction")
        results[0].observation = "green"
        results[0].status = "failed"
        check(SceneDiagnosticCase.conclusion(results) == "baseline_not_reproduced_or_unconfirmed", "never accept failed baseline")
        let redacted = SceneColorDiagnostics.sanitized("/Users/test/private token=secret password=hidden https://host/?key=hidden", home: "/Users/test")
        check(!redacted.contains("secret") && !redacted.contains("hidden") && !redacted.contains("/Users/test"), "log redaction")
        let properties = SceneColorDiagnostics.redactedProperties(["rgb": false, "color": "1 0.5 0", "text": "private text", "file": ["type": "scenetexture", "value": "/Users/test/photo.png"]]) as! [String: Any]
        check(properties["text"] as? String == "<REDACTED>", "redact custom text")
        check(properties["color"] as? String == "1 0.5 0", "preserve diagnostic color")
        check((properties["file"] as? [String: String])?["value"] == "<REDACTED>", "redact custom files")
        let root = FileManager.default.temporaryDirectory.appending(path: "scene-diagnostic-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: root) }
        for directory in ["frameworks", "payload/baseline", "payload/patched", "assets"] {
            try FileManager.default.createDirectory(at: root.appending(path: directory), withIntermediateDirectories: true)
        }
        for file in ["frameworks/libMoltenVK.dylib", "payload/baseline/libMoltenVK.dylib", "payload/patched/libMoltenVK.dylib", "scene.pkg"] {
            try Data("fixture".utf8).write(to: root.appending(path: file))
        }
        try Data("{\"schema\":1}".utf8).write(to: root.appending(path: "payload/manifest.json"))
        let executable = root.appending(path: "renderer")
        try "#!/bin/sh\nprintf 'token=secret\\n'\nprintf ready > \"$SCENERENDERER_DIAGNOSTICS_DIR/ready\"\nprintf captured > \"$SCENERENDERER_DIAGNOSTICS_DIR/captured\"\nexec /bin/sleep 30\n".write(to: executable, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: executable.path)
        let request = SceneDiagnosticRequest(package: root.appending(path: "scene.pkg"), assets: root.appending(path: "assets"), renderer: executable,
            frameworks: root.appending(path: "frameworks"), payload: root.appending(path: "payload"), properties: Data("{\"rgb\":false}".utf8),
            runtime: Data("{\"speed\":1,\"scriptStorage\":{\"private\":\"secret\"}}".utf8), arguments: [], metadata: [:], fastMath: "0", metalFX: false)
        let runner = SceneColorDiagnostics(request: request)
        runner.start()
        for _ in 0..<300 {
            if runner.ready {
                runner.observe(runner.cases.first(where: { $0.status == "running" })?.id == "B" ? "normal" : "green")
            }
            if runner.finished { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        check(runner.finished && !runner.running, "process orchestration finishes")
        check(runner.cases.filter { $0.status == "completed" }.count == 3, "run nonredundant cases")
        check(SceneDiagnosticCase.conclusion(runner.cases) == "controlled_patch_supported_not_yet_release_verified", "orchestration attribution")
        let report = runner.reportURL!
        let reportText = try String(contentsOf: report.appending(path: "report.json"), encoding: .utf8)
        check(!reportText.contains("secret"), "exclude script storage")
        let log = try String(contentsOf: report.appending(path: "A/renderer.log"), encoding: .utf8)
        check(!log.contains("secret") && log.contains("REDACTED"), "export only redacted logs")
        let archive = root.appending(path: "report.zip")
        try await runner.archiveReport(to: archive)
        let listing = Process()
        let listingPipe = Pipe()
        listing.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
        listing.arguments = ["-Z1", archive.path]
        listing.standardOutput = listingPipe
        try listing.run()
        let entries = String(decoding: listingPipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        listing.waitUntilExit()
        check(listing.terminationStatus == 0 && entries.contains("Report/report.json"), "valid exported archive")
        check(!entries.contains("cache-") && !entries.contains("runtime.json") && !entries.contains("properties.json"), "archive excludes private inputs")
        runner.cleanup()
        check(!FileManager.default.fileExists(atPath: report.path), "session cleanup")
        let noRepro = SceneColorDiagnostics(request: request)
        noRepro.start()
        for _ in 0..<200 {
            if noRepro.ready { noRepro.observe("normal") }
            if noRepro.finished { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        check(noRepro.finished && noRepro.cases[1].reason == "baseline_not_reproduced_or_unconfirmed", "stop unnecessary comparisons")
        noRepro.cleanup()
        let cancel = SceneColorDiagnostics(request: request)
        cancel.start()
        for _ in 0..<100 {
            if cancel.ready { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        cancel.cancel()
        for _ in 0..<100 {
            if cancel.finished { break }
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        check(cancel.finished && !cancel.running, "cancellation reaps child")
        cancel.cleanup()
        print("Scene diagnostics: \(checks) checks passed")
    }
}
