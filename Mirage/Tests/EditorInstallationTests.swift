//
//  EditorInstallationTests.swift
//  Mirage Wallpaper
//
//  Created on 2026/10/28.
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

func L(_ key: String, _ arguments: CVarArg...) -> String { key }

@main struct EditorInstallationTests {
    static func main() throws {
        let fm = FileManager.default
        let root = EditorInstallation.root
        try EditorInstallation.validateRoot()
        if CommandLine.arguments.contains("--assemble") {
            let catalog = try EditorInstallation.catalog()
            try EditorInstallation.assemble(catalog: catalog)
            try EditorInstallation.verifyInstalled(catalog: catalog)
            print("Clean-prefix assembly and installed binary verification passed.")
            return
        }
        for invalid in ["../outside", "/tmp/outside", "a/../../outside", "a\\..\\outside"] {
            do { _ = try EditorInstallation.contained(invalid, in: root); fatalError("Accepted unsafe path") }
            catch is EditorServiceError { }
        }
        let projects = root.appendingPathComponent("Projects")
        try fm.createDirectory(at: projects, withIntermediateDirectories: true)
        let project = projects.appendingPathComponent("keep.json")
        try Data("user project".utf8).write(to: project)
        let active = EditorInstallation.active
        try fm.createDirectory(at: active, withIntermediateDirectories: true)
        try Data("editor".utf8).write(to: active.appendingPathComponent("program"))
        try fm.createSymbolicLink(at: active.appendingPathComponent("projects"), withDestinationURL: projects)
        let cache = active.appendingPathComponent("runtime/logs")
        try fm.createDirectory(at: cache, withIntermediateDirectories: true)
        try Data(repeating: 1, count: 8192).write(to: cache.appendingPathComponent(".hidden-log"))
        try fm.createDirectory(at: EditorInstallation.downloads, withIntermediateDirectories: true)
        try Data(repeating: 1, count: 4096).write(to: EditorInstallation.downloads.appendingPathComponent("partial"))
        let preferences = root.appendingPathComponent("UserData/editor/base/Default/Local Storage")
        try fm.createDirectory(at: preferences, withIntermediateDirectories: true)
        try Data("editor preferences".utf8).write(to: preferences.appendingPathComponent("settings"))
        let profileCache = root.appendingPathComponent("UserData/editor/base/Default/Code Cache")
        try fm.createDirectory(at: profileCache, withIntermediateDirectories: true)
        try Data(repeating: 2, count: 4096).write(to: profileCache.appendingPathComponent("compiled"))
        let usage = EditorInstallation.usage()
        guard usage.reclaimable > 0 && usage.uninstallable >= usage.reclaimable else { throw NSError(domain: "Disk accounting: \(usage)", code: 1) }
        try EditorInstallation.clean(uninstall: false)
        precondition(fm.fileExists(atPath: active.appendingPathComponent("program").path))
        precondition(fm.fileExists(atPath: project.path))
        precondition(!fm.fileExists(atPath: EditorInstallation.downloads.path))
        try EditorInstallation.clean(uninstall: true)
        precondition(!fm.fileExists(atPath: active.path))
        precondition(fm.fileExists(atPath: preferences.appendingPathComponent("settings").path))
        precondition(!fm.fileExists(atPath: profileCache.path))
        let retained = try String(contentsOf: project, encoding: .utf8)
        precondition(retained == "user project")
        try fm.createSymbolicLink(at: active, withDestinationURL: projects)
        do { try EditorInstallation.clean(uninstall: true); fatalError("Accepted substituted installation directory") }
        catch is EditorServiceError { }
        precondition(fm.fileExists(atPath: project.path))
        print("Path containment, hidden cache accounting, cleanup, uninstall and project preservation passed.")
    }
}
