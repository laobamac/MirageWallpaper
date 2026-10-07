import Foundation
import Darwin

private let copyrightNotice = "Copyright © 2026 王孝慈. All rights reserved."

@main
struct SharedRuntimeSandboxProbe {
    static func main() throws {
        let contents = Bundle.main.bundleURL.appendingPathComponent("Contents")
        let assets = contents.appendingPathComponent("Resources/assets")
        let names = try FileManager.default.contentsOfDirectory(atPath: assets.path)
        guard !names.isEmpty else { throw NSError(domain: "EmptyAssets", code: 1) }
        let icd = contents.appendingPathComponent("Resources/vulkan/icd.d/MoltenVK_icd.json")
        _ = try Data(contentsOf: icd)
        let lib = contents.appendingPathComponent("Frameworks/libMirageSceneSaver.dylib")
        guard let handle = dlopen(lib.path, RTLD_NOW | RTLD_LOCAL) else {
            throw NSError(domain: String(cString: dlerror()), code: 2)
        }
        defer { dlclose(handle) }
        guard dlsym(handle, "MirageSceneDesktopCreateWithRuntime") != nil else {
            throw NSError(domain: "MissingNativeEntryPoint", code: 3)
        }
        guard getenv("APP_SANDBOX_CONTAINER_ID") != nil else {
            throw NSError(domain: "SandboxNotActive", code: 4)
        }
        print("PASS: App Sandbox active; shared extension assets, Vulkan ICD and native scene library accessible")
    }
}
