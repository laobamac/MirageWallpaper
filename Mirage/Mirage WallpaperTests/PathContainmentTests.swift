//
//  Mirage WallpaperTests
//
//  Characterization tests for paths supplied by untrusted wallpaper manifests.
//

import Foundation
import XCTest

final class PathContainmentTests: XCTestCase {
    private var sandbox: URL!
    private var root: URL!
    private var outside: URL!

    override func setUpWithError() throws {
        sandbox = FileManager.default.temporaryDirectory
            .appending(path: "MiragePathContainmentTests-\(UUID().uuidString)", directoryHint: .isDirectory)
        root = sandbox.appending(path: "wallpaper", directoryHint: .isDirectory)
        outside = sandbox.appending(path: "outside", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: outside, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        if let sandbox {
            try? FileManager.default.removeItem(at: sandbox)
        }
        sandbox = nil
        root = nil
        outside = nil
    }

    func testAcceptsNestedPathInsideRoot() {
        let result = PathContainment.containedURL("media/video.mp4", in: root)

        XCTAssertEqual(result?.path, root.appending(path: "media/video.mp4").path)
    }

    func testAcceptsEmptyPathAsRoot() {
        let result = PathContainment.containedURL("", in: root)

        XCTAssertEqual(result?.path, root.resolvingSymlinksInPath().path)
    }

    func testRejectsParentTraversal() {
        XCTAssertNil(PathContainment.containedURL("../outside/secret.txt", in: root))
        XCTAssertFalse(PathContainment.isContained("nested/../../outside/secret.txt", in: root))
    }

    func testRejectsSiblingWithSharedPathPrefix() {
        let sibling = sandbox.appending(path: "wallpaper-copy", directoryHint: .isDirectory)

        XCTAssertNil(PathContainment.containedURL("../wallpaper-copy/secret.txt", in: root))
        XCTAssertTrue(sibling.path.hasPrefix(root.path))
    }

    func testRejectsSymlinkThatEscapesRoot() throws {
        let escape = root.appending(path: "escape")
        try FileManager.default.createSymbolicLink(at: escape, withDestinationURL: outside)

        XCTAssertNil(PathContainment.containedURL("escape/secret.txt", in: root))
    }

    func testResolvesSafeSymlinkInsideRoot() throws {
        let media = root.appending(path: "media", directoryHint: .isDirectory)
        let alias = root.appending(path: "alias")
        try FileManager.default.createDirectory(at: media, withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: media)

        let result = PathContainment.containedURL("alias/video.mp4", in: root)

        XCTAssertEqual(result?.path, media.appending(path: "video.mp4").path)
    }

    func testPreservingShapeKeepsVisibleSymlinkRoot() throws {
        let realRoot = sandbox.appending(path: "real-wallpaper", directoryHint: .isDirectory)
        let visibleRoot = sandbox.appending(path: "visible-wallpaper")
        try FileManager.default.createDirectory(at: realRoot, withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(at: visibleRoot, withDestinationURL: realRoot)

        let result = PathContainment.containedURLPreservingShape("scene.pkg", in: visibleRoot)

        XCTAssertEqual(result?.path, visibleRoot.appending(path: "scene.pkg").path)
    }
}
