//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import UniformTypeIdentifiers

// Drives the 小组件 (widgets) tab: the installed rmskin theme list plus
// LoadType / Version filtering, search, apply, import and delete.
// Supports multiple simultaneously-applied themes (one RmskinWallpaper
// process per theme).
final class RmskinViewModel: ObservableObject, DropDelegate {

    // One controller per concurrently-applied theme.
    var activeControllers: [String: RmskinController] = [:]
    @Published var appliedThemeIDs: Set<String> = []

    @Published var themes: [RmskinTheme] = []
    @Published var searchText: String = ""
    @Published var selectedLoadTypes: Set<String> = []   // empty = all
    @Published var selectedVersions: Set<String> = []     // empty = all
    @Published var selectedTheme: RmskinTheme?

    @Published var importError: RmskinImportError?
    @Published var isImportErrorPresented = false

    private let appliedKey = "AppliedRmskinThemes"   // now stores an array

    init() {
        if let ids = UserDefaults.standard.stringArray(forKey: appliedKey) {
            appliedThemeIDs = Set(ids)
        }
        // 延迟首次加载，避免在窗口初始化 display cycle 期间
        // 触发 @Published 更新导致约束循环
        DispatchQueue.main.async { [weak self] in
            self?.refresh()
        }
    }

    // MARK: - Derived facets

    var availableLoadTypes: [String] {
        Array(Set(themes.map { $0.loadType }.filter { !$0.isEmpty })).sorted()
    }
    var availableVersions: [String] {
        Array(Set(themes.map { $0.version }.filter { !$0.isEmpty })).sorted()
    }

    var filteredThemes: [RmskinTheme] {
        themes.filter { theme in
            if !selectedLoadTypes.isEmpty, !selectedLoadTypes.contains(theme.loadType) { return false }
            if !selectedVersions.isEmpty, !selectedVersions.contains(theme.version) { return false }
            let q = searchText.trimmingCharacters(in: .whitespaces).lowercased()
            if !q.isEmpty {
                let hay = "\(theme.name) \(theme.author) \(theme.loadType) \(theme.version)".lowercased()
                if !hay.contains(q) { return false }
            }
            return true
        }
        .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    // MARK: - Actions

    func refresh() {
        DispatchQueue.global(qos: .userInitiated).async {
            let loaded = RmskinLibrary.shared.loadAll()
            DispatchQueue.main.async {
                self.themes = loaded
                if self.selectedTheme == nil { self.selectedTheme = loaded.first }
                // Re-apply any theme that was active before but whose process
                // may have died (e.g. after a restart).
                self.restoreActiveThemesIfNeeded()
            }
        }
    }

    /// Apply a theme — spawn a dedicated RmskinWallpaper process.
    /// Does NOT stop other running themes; each theme lives independently.
    func apply(_ theme: RmskinTheme) {
        // Idempotency is keyed on the actual running controller, NOT on
        // appliedThemeIDs — the latter is preloaded from UserDefaults at
        // launch so we can auto-restore themes from the previous session.
        if activeControllers[theme.id] != nil { return }

        let controller = RmskinController()
        controller.onExit = { [weak self] abnormal in
            if abnormal { NSLog("[Mirage] 小组件 \(theme.name) 渲染进程异常退出") }
            DispatchQueue.main.async {
                self?.activeControllers.removeValue(forKey: theme.id)
                self?.appliedThemeIDs.remove(theme.id)
                self?.saveApplied()
            }
        }
        guard controller.apply(theme) else { return }

        appliedThemeIDs.insert(theme.id)
        activeControllers[theme.id] = controller
        saveApplied()
    }

    /// Stop a specific theme's renderer process.
    func stop(_ theme: RmskinTheme) {
        guard let c = activeControllers[theme.id] else { return }
        c.stop()
        activeControllers.removeValue(forKey: theme.id)
        appliedThemeIDs.remove(theme.id)
        saveApplied()
    }

    /// Stop all running widget processes. Does NOT forget which themes
    /// were active — appliedThemeIDs is preserved so they auto-restore
    /// on next app launch.
    func stopAll() {
        for (_, c) in activeControllers { c.stop() }
        activeControllers.removeAll()
    }

    func isApplied(_ theme: RmskinTheme) -> Bool { appliedThemeIDs.contains(theme.id) }

    func delete(_ theme: RmskinTheme) {
        if isApplied(theme) { stop(theme) }
        try? RmskinLibrary.shared.delete(theme)
        refresh()
    }

    func importThemes(urls: [URL]) {
        DispatchQueue.global(qos: .userInitiated).async {
            var lastError: RmskinImportError?
            for url in urls {
                do { try RmskinLibrary.shared.importAny(at: url) }
                catch let e as RmskinImportError { lastError = e }
                catch { lastError = .copyFailed(error.localizedDescription) }
            }
            DispatchQueue.main.async {
                if let e = lastError {
                    self.importError = e
                    self.isImportErrorPresented = true
                }
                self.refresh()
            }
        }
    }

    func resetFilters() {
        selectedLoadTypes.removeAll()
        selectedVersions.removeAll()
    }

    // MARK: - Persistence

    private func saveApplied() {
        UserDefaults.standard.set(Array(appliedThemeIDs), forKey: appliedKey)
    }

    private func restoreActiveThemesIfNeeded() {
        // Re-apply themes that were active in a previous session.
        for id in appliedThemeIDs {
            guard activeControllers[id] == nil else { continue }
            guard let theme = themes.first(where: { $0.id == id }) else { continue }
            apply(theme)
        }
    }

    // MARK: - Drag & drop (.rmskin)

    func dropUpdated(info: DropInfo) -> DropProposal? { DropProposal(operation: .copy) }

    func performDrop(info: DropInfo) -> Bool {
        let providers = info.itemProviders(for: [UTType.fileURL])
        guard !providers.isEmpty else { return false }
        var urls: [URL] = []
        let group = DispatchGroup()
        for provider in providers {
            group.enter()
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                defer { group.leave() }
                if let data = item as? Data, let url = URL(dataRepresentation: data, relativeTo: nil) {
                    urls.append(url)
                }
            }
        }
        group.notify(queue: .main) { [weak self] in self?.importThemes(urls: urls) }
        return true
    }
}
