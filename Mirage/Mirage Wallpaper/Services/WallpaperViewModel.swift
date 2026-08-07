//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import CoreGraphics

struct WallpaperRuntimeState: Codable, Equatable {
    var volume: Float = 1.0
    var speed: Float = 1.0
    var muted: Bool = false
    var fillMode: FillMode = .cover
    var propertyOverrides: [String: WEPropertyValue] = [:]
}

struct DisplayWallpaperState: Codable, Equatable {
    var wallpaper: WEWallpaper
    var runtime: WallpaperRuntimeState
}

class WallpaperViewModel: ObservableObject {
    let renderer = RendererController()

    private struct AppliedPlaybackState: Equatable {
        let paused: Bool
        let muted: Bool
        let volume: Float
        let speed: Float
        let powerState: MiragePowerState
        let fps: Int
    }

    private struct PendingAssignmentProposal {
        let id: UUID
        let key: DisplayKey
        let state: DisplayWallpaperState
        let restoreFocus: Bool
    }

    private struct FailedAssignmentRecovery {
        let proposalID: UUID
        let committedAssignmentID: UUID
        let displayID: CGDirectDisplayID
        var waiterID: UUID?
    }

    private static let assignmentsDefaultsKey = "DisplayAssignments"
    private static let selectedDisplayDefaultsKey = "SelectedDisplay"
    private static let legacyWallpaperDefaultsKey = "CurrentWallpaper"

    @Published private(set) var displayStates: [DisplayKey: DisplayWallpaperState] = [:]

    @Published var selectedDisplayKey: DisplayKey {
        didSet {
            guard selectedDisplayKey != oldValue else { return }
            UserDefaults.standard.set(selectedDisplayKey.rawValue,
                                      forKey: Self.selectedDisplayDefaultsKey)
            syncStatusItems()
        }
    }

    @Published var currentByScreen: [Int: WEWallpaper] = [:]

    private var pendingScreenAssignments: [CGDirectDisplayID: UUID] = [:]
    private var pendingAssignmentProposals: [CGDirectDisplayID: [PendingAssignmentProposal]] = [:]
    private var failedAssignmentRecoveries: [DisplayKey: FailedAssignmentRecovery] = [:]
    private var committedAssignmentIDs: [DisplayKey: UUID] = [:]
    private var lastAppliedPlayback: [DisplayKey: AppliedPlaybackState] = [:]
    private var stoppedByPlaybackPolicy = false
    private var statesSaveWorkItem: DispatchWorkItem?
    private var runtimeSaveWorkItems: [DisplayKey: DispatchWorkItem] = [:]
    private var playbackCommandWorkItems: [DisplayKey: DispatchWorkItem] = [:]
    private var propertyCommandWorkItems: [DisplayKey: DispatchWorkItem] = [:]
    private var pendingPropertyCommands: [DisplayKey: [String: WEProjectProperty]] = [:]
    private var lastVolumeByDisplay: [DisplayKey: Float] = [:]
    private var lastRateByDisplay: [DisplayKey: Float] = [:]

    static var invalidWallpaper: WEWallpaper {
        WEWallpaper(using: .invalid,
                    where: Bundle.main.url(forResource: "WallpaperNotFound", withExtension: "mp4")
                        ?? URL(fileURLWithPath: "/dev/null"))
    }

    init() {
        let registry = DisplayRegistry.shared
        let stored = UserDefaults.standard.string(forKey: Self.selectedDisplayDefaultsKey)
            .map(DisplayKey.init(rawValue:))
        let connectedKeys = registry.connectedKeys
        if let stored, connectedKeys.contains(stored) {
            selectedDisplayKey = stored
        } else {
            selectedDisplayKey = registry.mainKey ?? DisplayKey(rawValue: "idx:0")
        }

        var loaded = Self.loadPersistedStates()
        if loaded.isEmpty, let migrated = Self.loadLegacyState(),
           let mainKey = registry.mainKey {
            loaded[mainKey] = migrated
        }
        displayStates = loaded
        committedAssignmentIDs = Dictionary(uniqueKeysWithValues: loaded.keys.map { ($0, UUID()) })

        renderer.isWallpaperTrusted = { WallpaperViewModel.isWallpaperTrusted($0) }
        NotificationCenter.default.addObserver(
            self, selector: #selector(displayTopologyChanged),
            name: NSApplication.didChangeScreenParametersNotification, object: nil)
        persistStates()
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    // MARK: 显示器状态

    var connectedDisplays: [DisplayInfo] {
        DisplayRegistry.shared.connected
    }

    var selectedDisplay: DisplayInfo? {
        DisplayRegistry.shared.info(for: selectedDisplayKey)
    }

    var selectedDisplayName: String {
        DisplayRegistry.shared.displayName(for: selectedDisplayKey)
    }

    var hasAnyWallpaper: Bool {
        !displayStates.isEmpty
    }

    func state(for key: DisplayKey) -> DisplayWallpaperState? {
        displayStates[key]
    }

    func wallpaper(for key: DisplayKey) -> WEWallpaper {
        displayStates[key]?.wallpaper ?? WallpaperViewModel.invalidWallpaper
    }

    func runtime(for key: DisplayKey) -> WallpaperRuntimeState {
        displayStates[key]?.runtime ?? WallpaperRuntimeState()
    }

    func isRendering(_ key: DisplayKey) -> Bool {
        guard let displayID = DisplayRegistry.shared.displayID(for: key) else { return false }
        return renderer.isRendering(onDisplay: displayID)
    }

    // MARK: 选中显示器的门面

    var currentWallpaper: WEWallpaper {
        get { wallpaper(for: selectedDisplayKey) }
        set { assign(newValue, to: selectedDisplayKey, restoreFocus: true) }
    }

    @discardableResult
    func updateStoredMetadata(title: String? = nil,
                              tags: [String]? = nil,
                              for key: DisplayKey? = nil) -> WEWallpaper? {
        let targetKey = key ?? selectedDisplayKey
        guard let current = displayStates[targetKey],
              let updated = current.wallpaper.updateStoredMetadata(title: title, tags: tags) else {
            return nil
        }
        let targetID = current.wallpaper.id
        var states = displayStates
        for displayKey in states.keys {
            guard states[displayKey]?.wallpaper.id == targetID else { continue }
            states[displayKey]?.wallpaper = updated
        }
        displayStates = states

        var currentWallpapers = currentByScreen
        for screen in currentWallpapers.keys {
            guard currentWallpapers[screen]?.id == targetID else { continue }
            currentWallpapers[screen] = updated
        }
        currentByScreen = currentWallpapers

        for displayID in pendingAssignmentProposals.keys {
            guard let proposals = pendingAssignmentProposals[displayID] else { continue }
            pendingAssignmentProposals[displayID] = proposals.map { proposal in
                guard proposal.state.wallpaper.id == targetID else { return proposal }
                var state = proposal.state
                state.wallpaper = updated
                return PendingAssignmentProposal(id: proposal.id, key: proposal.key, state: state,
                                                restoreFocus: proposal.restoreFocus)
            }
        }
        persistStates()
        return updated
    }

    var runtime: WallpaperRuntimeState {
        get { runtime(for: selectedDisplayKey) }
        set {
            guard var state = displayStates[selectedDisplayKey] else { return }
            state.runtime = Self.normalizedRuntime(newValue, for: state.wallpaper)
            displayStates[selectedDisplayKey] = state
            persistStates()
        }
    }

    var playVolume: Float {
        get { runtime(for: selectedDisplayKey).volume }
        set { setVolume(newValue, for: selectedDisplayKey) }
    }

    var playRate: Float {
        get { runtime(for: selectedDisplayKey).speed }
        set { setSpeed(newValue, for: selectedDisplayKey) }
    }

    // MARK: 全局设置桥接

    private var masterVolume: Float {
        Float(AppDelegate.shared.globalSettingsViewModel.settings.masterVolume)
    }
    private var globalMuted: Bool {
        AppDelegate.shared.globalSettingsViewModel.settings.globalMuted
    }
    private var globalFps: Int {
        Int(AppDelegate.shared.globalSettingsViewModel.settings.fps)
    }
    private var enableSpectrum: Bool {
        AppDelegate.shared.globalSettingsViewModel.settings.enableSpectrum
    }
    private var currentPlaybackPolicy: GSPlayback {
        AppDelegate.shared.globalSettingsViewModel.effectivePlaybackAction
    }

    // MARK: 信任（网页壁纸安全确认）

    private static let sessionTrustLock = NSLock()
    nonisolated(unsafe) private static var sessionTrusted: Set<String> = []

    static func trustForSession(_ w: WEWallpaper) {
        sessionTrustLock.lock()
        sessionTrusted.insert(w.id)
        sessionTrustLock.unlock()
    }

    static func clearSessionTrust() {
        sessionTrustLock.lock()
        sessionTrusted.removeAll()
        sessionTrustLock.unlock()
    }

    static func isWallpaperTrusted(_ w: WEWallpaper) -> Bool {
        let list = UserDefaults.standard.stringArray(forKey: "TrustedWallpapers") ?? []
        if list.contains(w.id) { return true }
        sessionTrustLock.lock()
        defer { sessionTrustLock.unlock() }
        return sessionTrusted.contains(w.id)
    }

    func isTrusted(_ w: WEWallpaper) -> Bool {
        WallpaperViewModel.isWallpaperTrusted(w)
    }

    func trust(_ w: WEWallpaper) {
        var list = UserDefaults.standard.stringArray(forKey: "TrustedWallpapers") ?? []
        if !list.contains(w.id) { list.append(w.id) }
        UserDefaults.standard.set(list, forKey: "TrustedWallpapers")
    }

    func trustAndApply(_ w: WEWallpaper) {
        trust(w)
        assign(w, to: selectedDisplayKey, restoreFocus: true)
    }

    func requestApply(_ wallpaper: WEWallpaper) {
        requestApply(wallpaper, to: selectedDisplayKey)
    }

    func requestApply(_ wallpaper: WEWallpaper, to key: DisplayKey) {
        guard wallpaper.isValid, wallpaper.kind != .unsupported else { return }
        if wallpaper.kind == .web, !isTrusted(wallpaper) {
            guard let displayID = DisplayRegistry.shared.displayID(for: key) else { return }
            AppDelegate.shared.contentViewModel.warningUnsafeWallpaperModal(
                which: wallpaper, action: .applyOnDisplay(displayID))
            return
        }
        assign(wallpaper, to: key, restoreFocus: true)
    }

    // MARK: 指派与停止

    func assign(_ wallpaper: WEWallpaper, to key: DisplayKey, restoreFocus: Bool = false) {
        guard wallpaper.isValid, wallpaper.kind != .unsupported else {
            clear(key)
            return
        }
        let previous = displayStates[key]
        let resolved: WallpaperRuntimeState
        if let previous, previous.wallpaper.id == wallpaper.id {
            resolved = previous.runtime
        } else {
            resolved = Self.loadPersistedRuntime(for: wallpaper)
        }
        let state = DisplayWallpaperState(wallpaper: wallpaper, runtime: resolved)
        cancelPendingProperties(for: key)
        let shouldRestoreFocus = restoreFocus && Thread.isMainThread && NSApp.isActive &&
            AppDelegate.shared.mainWindowController?.window?.isVisible == true

        guard let displayID = DisplayRegistry.shared.displayID(for: key) else {
            discardPendingAssignment(for: key)
            commitAssignmentState(state, assignmentID: UUID(), for: key)
            if shouldRestoreFocus { AppDelegate.shared.restoreMainWindowFocus() }
            syncStatusItems()
            return
        }
        if currentPlaybackPolicy == .stop {
            pendingScreenAssignments[displayID] = nil
            pendingAssignmentProposals[displayID] = nil
            commitAssignmentState(state, assignmentID: UUID(), for: key)
            if shouldRestoreFocus { AppDelegate.shared.restoreMainWindowFocus() }
        } else {
            submitAssignment(state, to: displayID, key: key, reuseActive: true,
                             restoreFocus: shouldRestoreFocus)
            if shouldRestoreFocus { AppDelegate.shared.restoreMainWindowFocus() }
        }
        syncStatusItems()
    }

    private func submitAssignment(_ state: DisplayWallpaperState,
                                  to displayID: CGDirectDisplayID,
                                  key: DisplayKey,
                                  reuseActive: Bool,
                                  restoreFocus: Bool) {
        let requestID = UUID()
        let proposal = PendingAssignmentProposal(id: requestID, key: key, state: state,
                                                 restoreFocus: restoreFocus)
        cancelFailedAssignmentRecovery(for: key)
        pendingAssignmentProposals[displayID, default: []].append(proposal)
        apply(state, to: displayID, key: key, reuseActive: reuseActive,
              assignmentID: proposal.id, requestID: requestID) { [weak self] success in
            self?.completeAssignment(proposal, on: displayID, success: success)
        }
    }

    private func completeAssignment(_ proposal: PendingAssignmentProposal,
                                    on displayID: CGDirectDisplayID,
                                    success: Bool) {
        guard var proposals = pendingAssignmentProposals[displayID],
              let index = proposals.firstIndex(where: { $0.id == proposal.id }) else { return }
        let wasLatest = proposals.last?.id == proposal.id
        proposals.remove(at: index)
        if proposals.isEmpty {
            pendingAssignmentProposals[displayID] = nil
        } else {
            pendingAssignmentProposals[displayID] = proposals
        }

        if proposal.restoreFocus {
            AppDelegate.shared.restoreMainWindowFocus()
        }

        if success {
            let committed = assignmentStateMergingCurrentRuntime(
                proposal.state, for: proposal.key)
            commitAssignmentState(committed, assignmentID: proposal.id, for: proposal.key)
            commit(committed, to: displayID, key: proposal.key)
            applyPlaybackPolicy(currentPlaybackPolicy, for: proposal.key, force: true)
        } else if wasLatest && proposals.isEmpty {
            reconcileCurrentWallpaper(on: displayID, key: proposal.key)
            recoverCommittedAssignmentIfUncovered(
                on: displayID, key: proposal.key, failedProposalID: proposal.id)
            if displayStates[proposal.key] != nil {
                applyPlaybackPolicy(currentPlaybackPolicy, for: proposal.key, force: true)
            }
        }
        syncStatusItems()
    }

    private func assignmentStateMergingCurrentRuntime(
        _ proposed: DisplayWallpaperState,
        for key: DisplayKey
    ) -> DisplayWallpaperState {
        var committed = proposed
        if let current = displayStates[key],
           current.wallpaper.id == committed.wallpaper.id {
            // Runtime controls may have changed while a same-wallpaper restart
            // was preparing. Never overwrite those newer edits with the
            // immutable snapshot captured when the request started.
            committed.runtime = current.runtime
        }
        return committed
    }

    private func hasPendingAssignment(on displayID: CGDirectDisplayID,
                                      for key: DisplayKey) -> Bool {
        pendingAssignmentProposals[displayID]?.contains { $0.key == key } ?? false
    }

    private func commitAssignmentState(_ state: DisplayWallpaperState,
                                       assignmentID: UUID,
                                       for key: DisplayKey) {
        if let previous = displayStates[key], previous.wallpaper.id != state.wallpaper.id {
            cancelRuntimeSave(for: key)
            persistRuntime(previous.runtime, for: previous.wallpaper)
        }
        displayStates[key] = state
        cancelFailedAssignmentRecovery(for: key)
        committedAssignmentIDs[key] = assignmentID
        lastAppliedPlayback[key] = nil
        persistStates()
    }

    private func ensureCommittedAssignmentID(for key: DisplayKey) -> UUID {
        if let assignmentID = committedAssignmentIDs[key] { return assignmentID }
        let assignmentID = UUID()
        committedAssignmentIDs[key] = assignmentID
        return assignmentID
    }

    private func discardPendingAssignment(for key: DisplayKey) {
        let displayIDs = Array(pendingAssignmentProposals.keys)
        for displayID in displayIDs {
            guard let proposals = pendingAssignmentProposals[displayID] else { continue }
            let removedIDs = Set(proposals.lazy.filter { $0.key == key }.map(\.id))
            guard !removedIDs.isEmpty else { continue }
            let remaining = proposals.filter { !removedIDs.contains($0.id) }
            pendingAssignmentProposals[displayID] = remaining.isEmpty ? nil : remaining
            if let pending = pendingScreenAssignments[displayID], removedIDs.contains(pending) {
                pendingScreenAssignments[displayID] = nil
            }
        }
    }

    func applyOnDisplay(_ w: WEWallpaper, displayID targetDisplayID: CGDirectDisplayID,
                        restoreFocus: Bool = true) {
        guard let key = DisplayRegistry.shared.key(forDisplay: targetDisplayID) else { return }
        assign(w, to: key, restoreFocus: restoreFocus)
    }

    func applyOnScreen(_ w: WEWallpaper, screen: Int, restoreFocus: Bool = false) {
        guard let key = DisplayRegistry.shared.key(forScreenIndex: screen) else { return }
        assign(w, to: key, restoreFocus: restoreFocus)
    }

    func applyToAllDisplays(_ w: WEWallpaper, restoreFocus: Bool = true) {
        guard w.isValid, w.kind != .unsupported else { return }
        for info in DisplayRegistry.shared.connected {
            assign(w, to: info.key, restoreFocus: restoreFocus)
        }
    }

    func applyToAllScreens() {
        guard let state = displayStates[selectedDisplayKey] else { return }
        applyToAllDisplays(state.wallpaper, restoreFocus: true)
    }

    func clear(_ key: DisplayKey) {
        discardPendingAssignment(for: key)
        if let state = displayStates[key] {
            cancelRuntimeSave(for: key)
            persistRuntime(state.runtime, for: state.wallpaper)
        }
        displayStates[key] = nil
        cancelFailedAssignmentRecovery(for: key)
        committedAssignmentIDs[key] = nil
        lastAppliedPlayback[key] = nil
        cancelPendingProperties(for: key)
        persistStates()
        if let index = DisplayRegistry.shared.screenIndex(for: key) {
            currentByScreen[index] = nil
        }
        if let displayID = DisplayRegistry.shared.displayID(for: key) {
            pendingScreenAssignments[displayID] = nil
            pendingAssignmentProposals[displayID] = nil
            renderer.stop(displayID: displayID)
        }
        syncStatusItems()
    }

    func stopWallpaper() {
        clear(selectedDisplayKey)
    }

    func stopAllWallpapers() {
        for (key, state) in displayStates {
            cancelRuntimeSave(for: key)
            persistRuntime(state.runtime, for: state.wallpaper)
            cancelPendingProperties(for: key)
        }
        cancelAllFailedAssignmentRecoveries()
        renderer.stopAll()
        pendingScreenAssignments.removeAll()
        pendingAssignmentProposals.removeAll()
        committedAssignmentIDs.removeAll()
        displayStates.removeAll()
        lastAppliedPlayback.removeAll()
        currentByScreen.removeAll()
        stoppedByPlaybackPolicy = false
        persistStates()
        syncStatusItems()
    }

    func removeWallpaper(at directory: URL) {
        let identifier = directory.path(percentEncoded: false)
        let targets = displayStates.filter { $0.value.wallpaper.id == identifier }.map(\.key)
        for key in targets { clear(key) }
    }

    // MARK: 渲染

    func restoreAllDisplays() {
        guard currentPlaybackPolicy != .stop else { return }
        for info in DisplayRegistry.shared.connected {
            guard !hasPendingAssignment(on: info.displayID, for: info.key),
                  let state = displayStates[info.key] else { continue }
            apply(state, to: info.displayID, key: info.key, reuseActive: false,
                  assignmentID: ensureCommittedAssignmentID(for: info.key))
        }
        applyPlaybackPolicy(currentPlaybackPolicy, force: true)
        syncStatusItems()
    }

    func reapply(for key: DisplayKey) {
        guard let state = displayStates[key],
              let displayID = DisplayRegistry.shared.displayID(for: key) else { return }
        cancelPendingProperties(for: key)
        if currentPlaybackPolicy != .stop {
            // A user-triggered reapply is itself an assignment transaction. If
            // another request is in flight this queues deterministically behind
            // it and keeps UI/persistence aligned with the eventual renderer.
            submitAssignment(state, to: displayID, key: key, reuseActive: false,
                             restoreFocus: false)
        }
        applyPlaybackPolicy(currentPlaybackPolicy, for: key, force: true)
    }

    func reapplyCurrent() {
        reapply(for: selectedDisplayKey)
    }

    private func apply(_ state: DisplayWallpaperState, to displayID: CGDirectDisplayID,
                       key: DisplayKey, reuseActive: Bool,
                       assignmentID: UUID,
                       requestID: UUID = UUID(),
                       onResult: ((Bool) -> Void)? = nil) {
        let options = makeRenderOptions(
            for: state.wallpaper, runtime: state.runtime, assignmentID: assignmentID)
        pendingScreenAssignments[displayID] = requestID
        let accepted = renderer.render(
            state.wallpaper,
            onDisplay: displayID,
            options: options,
            reuseActive: reuseActive
        ) { [weak self] success in
            guard let self else { return }
            if let onResult {
                if self.pendingScreenAssignments[displayID] == requestID {
                    self.pendingScreenAssignments[displayID] = nil
                }
                onResult(success)
                return
            }
            guard self.pendingScreenAssignments[displayID] == requestID else { return }
            self.pendingScreenAssignments[displayID] = nil
            if success {
                self.commit(state, to: displayID, key: key)
            }
        }
        if !accepted, pendingScreenAssignments[displayID] == requestID {
            pendingScreenAssignments[displayID] = nil
            onResult?(false)
        }
    }

    private func commit(_ state: DisplayWallpaperState, to displayID: CGDirectDisplayID,
                        key: DisplayKey) {
        if let index = DisplayRegistry.shared.screenIndex(for: key) {
            currentByScreen[index] = state.wallpaper
        }
        DesktopOverrideService.shared.scheduleCapture(
            forDisplay: displayID, wallpaper: state.wallpaper)
    }

    private func reconcileCurrentWallpaper(on displayID: CGDirectDisplayID,
                                           key: DisplayKey) {
        guard let index = DisplayRegistry.shared.screenIndex(for: key) else { return }
        currentByScreen[index] = renderer.currentWallpaper(onDisplay: displayID)
    }

    private func recoverCommittedAssignmentIfUncovered(
        on displayID: CGDirectDisplayID,
        key: DisplayKey,
        failedProposalID: UUID
    ) {
        cancelFailedAssignmentRecovery(for: key)
        guard currentPlaybackPolicy != .stop,
              DisplayRegistry.shared.displayID(for: key) == displayID,
              displayStates[key] != nil,
              !hasPendingAssignment(on: displayID, for: key) else { return }
        let committedAssignmentID = ensureCommittedAssignmentID(for: key)
        if renderer.isRendering(onDisplay: displayID) { return }

        failedAssignmentRecoveries[key] = FailedAssignmentRecovery(
            proposalID: failedProposalID,
            committedAssignmentID: committedAssignmentID,
            displayID: displayID,
            waiterID: nil)
        let waiterID = renderer.whenCoverageOrWorkSettles(
            onDisplay: displayID
        ) { [weak self] in
            self?.finishFailedAssignmentRecovery(
                for: key, failedProposalID: failedProposalID,
                committedAssignmentID: committedAssignmentID, displayID: displayID)
        }
        guard var recovery = failedAssignmentRecoveries[key],
              recovery.proposalID == failedProposalID,
              recovery.committedAssignmentID == committedAssignmentID,
              recovery.displayID == displayID else {
            renderer.cancelCoverageOrWorkWaiter(waiterID, onDisplay: displayID)
            return
        }
        recovery.waiterID = waiterID
        failedAssignmentRecoveries[key] = recovery
    }

    private func finishFailedAssignmentRecovery(
        for key: DisplayKey,
        failedProposalID: UUID,
        committedAssignmentID: UUID,
        displayID: CGDirectDisplayID
    ) {
        guard let recovery = failedAssignmentRecoveries[key],
              recovery.proposalID == failedProposalID,
              recovery.committedAssignmentID == committedAssignmentID,
              recovery.displayID == displayID else { return }
        failedAssignmentRecoveries[key] = nil
        if renderer.isRendering(onDisplay: displayID) { return }
        guard currentPlaybackPolicy != .stop,
              DisplayRegistry.shared.displayID(for: key) == displayID,
              committedAssignmentIDs[key] == committedAssignmentID,
              let state = displayStates[key],
              !hasPendingAssignment(on: displayID, for: key),
              !renderer.hasCoverageOrWork(onDisplay: displayID) else { return }
        // This fallback is deliberately non-transactional: a second failure
        // ends without registering another recovery waiter.
        apply(state, to: displayID, key: key, reuseActive: true,
              assignmentID: committedAssignmentID)
    }

    private func cancelFailedAssignmentRecovery(for key: DisplayKey) {
        guard let recovery = failedAssignmentRecoveries.removeValue(forKey: key),
              let waiterID = recovery.waiterID else { return }
        renderer.cancelCoverageOrWorkWaiter(waiterID, onDisplay: recovery.displayID)
    }

    private func cancelAllFailedAssignmentRecoveries() {
        for key in Array(failedAssignmentRecoveries.keys) {
            cancelFailedAssignmentRecovery(for: key)
        }
    }

    private func makeRenderOptions(for w: WEWallpaper,
                                   runtime state: WallpaperRuntimeState,
                                   assignmentID: UUID,
                                   playbackAction: GSPlayback? = nil) -> RenderOptions {
        let settings = AppDelegate.shared.globalSettingsViewModel.settings
        let action = playbackAction ?? currentPlaybackPolicy
        var opts = RenderOptions()
        opts.assignmentID = assignmentID
        opts.fps = globalFps
        opts.enableSpectrum = enableSpectrum
        opts.muted = state.muted || globalMuted || state.volume == 0 || action == .mute
        opts.volume = state.volume * masterVolume
        opts.speed = state.speed
        opts.fillMode = state.fillMode
        opts.userProperties = effectiveProperties(for: w, runtime: state)
        let throttledFps = AppDelegate.shared.globalSettingsViewModel
            .throttledFps(base: globalFps)
        let paused = state.speed == 0 || action == .pause
        opts.powerState = paused ? .pause
            : (throttledFps < globalFps ? .throttle : .run)
        opts.powerFps = throttledFps
        opts.loadFromMemory = (settings.wallpaperLoadSource ?? .disk) == .memory
        switch settings.textureResolution {
        case .highQuality: opts.renderScale = 1.0
        case .automatic: opts.renderScale = 0.75
        case .highPerformance: opts.renderScale = 0.5
        }
        switch settings.antiAliasing {
        case .none: opts.msaaSamples = 1
        case .msaa_x2: opts.msaaSamples = 2
        case .msaa_x4: opts.msaaSamples = 4
        case .msaa_x8: opts.msaaSamples = 8
        }
        return opts
    }

    // MARK: 拓扑变化

    @objc private func displayTopologyChanged() {
        DisplayRegistry.shared.invalidate()
        let connected = DisplayRegistry.shared.connected
        let connectedIDs = Set(connected.map(\.displayID))
        let connectedKeys = Set(connected.map(\.key))

        let staleRecoveryKeys = failedAssignmentRecoveries.compactMap { key, recovery in
            !connectedKeys.contains(key) ||
                DisplayRegistry.shared.displayID(for: key) != recovery.displayID ? key : nil
        }
        for key in staleRecoveryKeys {
            cancelFailedAssignmentRecovery(for: key)
        }

        renderer.stopDisplays(except: connectedIDs)
        pendingScreenAssignments = pendingScreenAssignments.filter { connectedIDs.contains($0.key) }
        pendingAssignmentProposals = pendingAssignmentProposals.filter {
            connectedIDs.contains($0.key)
        }
        lastAppliedPlayback = lastAppliedPlayback.filter { connectedKeys.contains($0.key) }

        if !connectedKeys.contains(selectedDisplayKey) {
            selectedDisplayKey = DisplayRegistry.shared.mainKey
                ?? connected.first?.key ?? selectedDisplayKey
        }

        let inherited = DisplayRegistry.shared.mainKey.flatMap { displayStates[$0] }
            ?? displayStates[selectedDisplayKey]
        var seeded = false
        for info in connected {
            if hasPendingAssignment(on: info.displayID, for: info.key) { continue }
            if displayStates[info.key] == nil, let inherited {
                if currentPlaybackPolicy == .stop {
                    displayStates[info.key] = inherited
                    committedAssignmentIDs[info.key] = UUID()
                    lastAppliedPlayback[info.key] = nil
                    seeded = true
                } else {
                    submitAssignment(inherited, to: info.displayID,
                                     key: info.key, reuseActive: true,
                                     restoreFocus: false)
                    continue
                }
            }
            guard let state = displayStates[info.key] else { continue }
            if currentPlaybackPolicy != .stop {
                apply(state, to: info.displayID, key: info.key, reuseActive: true,
                      assignmentID: ensureCommittedAssignmentID(for: info.key))
            }
        }
        if seeded { persistStates() }

        var rebuilt: [Int: WEWallpaper] = [:]
        for info in connected {
            guard let wallpaper = renderer.currentWallpaper(onDisplay: info.displayID) else { continue }
            rebuilt[info.index] = wallpaper
        }
        currentByScreen = rebuilt

        DesktopOverrideService.shared.scheduleCaptureForAllScreens()
        if !stoppedByPlaybackPolicy {
            applyPlaybackPolicy(currentPlaybackPolicy, force: true)
        }
        syncStatusItems()
    }

    // MARK: 运行时状态持久化

    private static func runtimeKey(for w: WEWallpaper) -> String { "Runtime_\(w.id)" }

    func loadRuntime(for w: WEWallpaper) -> WallpaperRuntimeState {
        Self.loadPersistedRuntime(for: w)
    }

    private static func loadPersistedRuntime(for w: WEWallpaper) -> WallpaperRuntimeState {
        if let data = UserDefaults.standard.data(forKey: runtimeKey(for: w)),
           let saved = try? JSONDecoder().decode(WallpaperRuntimeState.self, from: data) {
            let normalized = normalizedRuntime(saved, for: w)
            if normalized != saved, let migrated = try? JSONEncoder().encode(normalized) {
                UserDefaults.standard.set(migrated, forKey: runtimeKey(for: w))
            }
            return normalized
        }
        return WallpaperRuntimeState()
    }

    func saveRuntime() {
        if var state = displayStates[selectedDisplayKey] {
            let normalized = Self.normalizedRuntime(state.runtime, for: state.wallpaper)
            if normalized != state.runtime {
                state.runtime = normalized
                displayStates[selectedDisplayKey] = state
            }
        }
        for (key, state) in displayStates {
            cancelRuntimeSave(for: key)
            persistRuntime(state.runtime, for: state.wallpaper)
        }
        persistStates()
    }

    private func persistRuntime(_ state: WallpaperRuntimeState, for wallpaper: WEWallpaper) {
        guard wallpaper.isValid else { return }
        let normalized = Self.normalizedRuntime(state, for: wallpaper)
        guard let data = try? JSONEncoder().encode(normalized) else { return }
        UserDefaults.standard.set(data, forKey: Self.runtimeKey(for: wallpaper))
        if ScreenSaverManager.shared.configuredWallpaperID() == wallpaper.id {
            try? ScreenSaverManager.shared.configure(
                with: wallpaper,
                runtime: normalized,
                properties: effectiveProperties(for: wallpaper, runtime: normalized),
                fps: Int(AppDelegate.shared.globalSettingsViewModel.settings.fps)
            )
        }
    }

    private func scheduleRuntimeSave(for key: DisplayKey) {
        runtimeSaveWorkItems[key]?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.runtimeSaveWorkItems[key] = nil
            guard let state = self.displayStates[key] else { return }
            self.persistRuntime(state.runtime, for: state.wallpaper)
        }
        runtimeSaveWorkItems[key] = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: work)
    }

    private func cancelRuntimeSave(for key: DisplayKey) {
        runtimeSaveWorkItems[key]?.cancel()
        runtimeSaveWorkItems[key] = nil
    }

    private func persistStates() {
        statesSaveWorkItem?.cancel()
        statesSaveWorkItem = nil
        writeStates()
    }

    private func scheduleStatesSave() {
        statesSaveWorkItem?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.statesSaveWorkItem = nil
            self.writeStates()
        }
        statesSaveWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: work)
    }

    private func writeStates() {
        var raw: [String: DisplayWallpaperState] = [:]
        for (key, state) in displayStates {
            raw[key.rawValue] = state
        }
        if let data = try? JSONEncoder().encode(raw) {
            UserDefaults.standard.set(data, forKey: Self.assignmentsDefaultsKey)
        }
        let mainState = DisplayRegistry.shared.mainKey.flatMap { displayStates[$0] }
        if let mainState, let data = try? JSONEncoder().encode(mainState.wallpaper) {
            UserDefaults.standard.set(data, forKey: Self.legacyWallpaperDefaultsKey)
        } else if mainState == nil {
            UserDefaults.standard.removeObject(forKey: Self.legacyWallpaperDefaultsKey)
        }
    }

    private static func loadPersistedStates() -> [DisplayKey: DisplayWallpaperState] {
        guard let data = UserDefaults.standard.data(forKey: assignmentsDefaultsKey),
              let raw = try? JSONDecoder().decode([String: DisplayWallpaperState].self, from: data)
        else { return [:] }
        var result: [DisplayKey: DisplayWallpaperState] = [:]
        for (rawKey, stored) in raw {
            guard FileManager.default.fileExists(
                atPath: stored.wallpaper.wallpaperDirectory.path) else { continue }
            let refreshed = WEWallpaper.load(from: stored.wallpaper.wallpaperDirectory)
            guard refreshed.isValid, refreshed.kind != .unsupported else { continue }
            result[DisplayKey(rawValue: rawKey)] = DisplayWallpaperState(
                wallpaper: refreshed,
                runtime: normalizedRuntime(stored.runtime, for: refreshed))
        }
        return result
    }

    private static func loadLegacyState() -> DisplayWallpaperState? {
        guard let data = UserDefaults.standard.data(forKey: legacyWallpaperDefaultsKey),
              let stored = try? JSONDecoder().decode(WEWallpaper.self, from: data),
              FileManager.default.fileExists(atPath: stored.wallpaperDirectory.path)
        else { return nil }
        let refreshed = WEWallpaper.load(from: stored.wallpaperDirectory)
        guard refreshed.isValid, refreshed.kind != .unsupported else { return nil }
        return DisplayWallpaperState(wallpaper: refreshed,
                                     runtime: loadPersistedRuntime(for: refreshed))
    }

    // MARK: 属性合并

    func effectiveProperties(for w: WEWallpaper) -> [String: WEProjectProperty] {
        effectiveProperties(for: w, runtime: runtime(for: selectedDisplayKey))
    }

    func effectiveProperties(for w: WEWallpaper,
                             runtime runtimeState: WallpaperRuntimeState) -> [String: WEProjectProperty] {
        var result = w.project.general?.properties?.items ?? [:]
        for (key, override) in runtimeState.propertyOverrides {
            if var prop = result[key] {
                prop.value = prop.normalizedComboValue(override)
                result[key] = prop
            }
        }
        if !w.assetOverlayDirectories.isEmpty {
            let baseProperties = loadBaseProperties(for: w)
            let presetKeys = Set(w.project.preset?.keys.map { $0 } ?? [])
            for (key, var property) in result where property.propertyType == .file ||
                property.propertyType == .scenetexture || property.propertyType == .directory {
                let path = property.value.stringValue
                guard !path.isEmpty else { continue }
                if isWindowsAbsolutePath(path) || ((path as NSString).isAbsolutePath &&
                    !FileManager.default.fileExists(atPath: path)) {
                    if presetKeys.contains(key), let fallback = baseProperties[key]?.value {
                        property.value = fallback
                        result[key] = property
                    }
                } else if !(path as NSString).isAbsolutePath,
                          let resolved = resolvedPresetAsset(path, in: w.assetOverlayDirectories) {
                    property.value = .string(resolved.path)
                    result[key] = property
                } else if !(path as NSString).isAbsolutePath,
                          presetKeys.contains(key), path.hasPrefix("files/"),
                          let fallback = baseProperties[key]?.value {
                    property.value = fallback
                    result[key] = property
                }
            }
        }
        return result
    }

    private static func normalizedRuntime(_ source: WallpaperRuntimeState,
                                          for wallpaper: WEWallpaper) -> WallpaperRuntimeState {
        var result = source
        let properties = wallpaper.project.general?.properties?.items ?? [:]
        for (key, value) in result.propertyOverrides {
            guard let property = properties[key] else { continue }
            result.propertyOverrides[key] = property.normalizedComboValue(value)
        }
        return result
    }

    private func loadBaseProperties(for wallpaper: WEWallpaper) -> [String: WEProjectProperty] {
        let url = wallpaper.renderDirectory.appending(path: "project.json")
        guard let data = try? Data(contentsOf: url),
              let project = try? JSONDecoder().decode(WEProject.self, from: data) else { return [:] }
        return project.general?.properties?.items ?? [:]
    }

    private func isWindowsAbsolutePath(_ path: String) -> Bool {
        path.range(of: "^[A-Za-z]:[\\\\/]", options: .regularExpression) != nil
    }

    private func resolvedPresetAsset(_ relativePath: String, in directories: [URL]) -> URL? {
        for directory in directories {
            let root = directory.standardizedFileURL.resolvingSymlinksInPath()
            let candidate = root.appending(path: relativePath).standardizedFileURL.resolvingSymlinksInPath()
            let isInside = candidate.path == root.path || candidate.path.hasPrefix(root.path + "/")
            if isInside, FileManager.default.fileExists(atPath: candidate.path) {
                return candidate
            }
        }
        return nil
    }

    // MARK: 属性实时下发

    func setProperty(key: String, value: WEPropertyValue) {
        setProperty(key: key, value: value, for: selectedDisplayKey)
    }

    func setProperty(key propertyKey: String, value: WEPropertyValue, for displayKey: DisplayKey) {
        guard let state = displayStates[displayKey],
              var prop = state.wallpaper.project.general?.properties?.items[propertyKey] else { return }
        let normalizedValue = prop.normalizedComboValue(value)
        prop.value = normalizedValue
        mutateRuntime(for: displayKey) { $0.propertyOverrides[propertyKey] = normalizedValue }

        switch state.wallpaper.kind {
        case .web, .scene:
            pendingPropertyCommands[displayKey, default: [:]][propertyKey] = prop
            propertyCommandWorkItems[displayKey]?.cancel()
            let wallpaperID = state.wallpaper.id
            let work = DispatchWorkItem { [weak self] in
                guard let self else { return }
                self.propertyCommandWorkItems[displayKey] = nil
                let commands = self.pendingPropertyCommands.removeValue(forKey: displayKey) ?? [:]
                guard let displayID = DisplayRegistry.shared.displayID(for: displayKey) else { return }
                var assignmentIDs: Set<UUID> = []
                if self.displayStates[displayKey]?.wallpaper.id == wallpaperID {
                    assignmentIDs.insert(self.ensureCommittedAssignmentID(for: displayKey))
                }
                for proposal in self.pendingAssignmentProposals[displayID] ?? []
                where proposal.key == displayKey && proposal.state.wallpaper.id == wallpaperID {
                    assignmentIDs.insert(proposal.id)
                }
                for (key, property) in commands {
                    for assignmentID in assignmentIDs {
                        self.renderer.setProperty(
                            key: key, property: property, onDisplay: displayID,
                            assignmentID: assignmentID)
                    }
                }
            }
            propertyCommandWorkItems[displayKey] = work
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0 / 60.0, execute: work)
        case .video, .unsupported:
            break
        }
    }

    private func cancelPendingProperties(for key: DisplayKey) {
        propertyCommandWorkItems[key]?.cancel()
        propertyCommandWorkItems[key] = nil
        pendingPropertyCommands[key] = nil
    }

    func setFillMode(_ mode: FillMode) {
        setFillMode(mode, for: selectedDisplayKey)
    }

    func setFillMode(_ mode: FillMode, for key: DisplayKey) {
        guard let state = displayStates[key] else { return }
        let wallpaperID = state.wallpaper.id
        mutateRuntime(for: key) { $0.fillMode = mode }
        guard let displayID = DisplayRegistry.shared.displayID(for: key) else { return }
        var assignmentIDs: Set<UUID> = [ensureCommittedAssignmentID(for: key)]
        for proposal in pendingAssignmentProposals[displayID] ?? []
        where proposal.key == key && proposal.state.wallpaper.id == wallpaperID {
            assignmentIDs.insert(proposal.id)
        }
        for assignmentID in assignmentIDs {
            renderer.setFillMode(
                mode, onDisplay: displayID, assignmentID: assignmentID)
        }
    }

    func resetProperties() {
        let key = selectedDisplayKey
        guard var state = displayStates[key] else { return }
        state.runtime = WallpaperRuntimeState()
        displayStates[key] = state
        lastAppliedPlayback[key] = nil
        persistStates()
        cancelRuntimeSave(for: key)
        persistRuntime(state.runtime, for: state.wallpaper)
        reapply(for: key)
        syncStatusItems()
    }

    // MARK: 播放控制

    private func mutateRuntime(for key: DisplayKey,
                               _ transform: (inout WallpaperRuntimeState) -> Void) {
        guard var state = displayStates[key] else { return }
        transform(&state.runtime)
        displayStates[key] = state
        scheduleStatesSave()
        scheduleRuntimeSave(for: key)
    }

    func setVolume(_ value: Float, for key: DisplayKey) {
        guard displayStates[key] != nil else { return }
        lastVolumeByDisplay[key] = runtime(for: key).volume
        mutateRuntime(for: key) { $0.volume = value }
        schedulePlaybackPolicyApplication(for: key)
        syncStatusItems()
    }

    func setSpeed(_ value: Float, for key: DisplayKey) {
        guard displayStates[key] != nil else { return }
        lastRateByDisplay[key] = runtime(for: key).speed
        mutateRuntime(for: key) { $0.speed = value }
        schedulePlaybackPolicyApplication(for: key)
        syncStatusItems()
    }

    func muteAll() {
        for key in Array(displayStates.keys) { setVolume(0, for: key) }
    }

    func unmuteAll() {
        for key in Array(displayStates.keys) {
            let remembered = lastVolumeByDisplay[key] ?? 1
            setVolume(remembered == 0 ? 1 : remembered, for: key)
        }
    }

    func pauseAll() {
        for key in Array(displayStates.keys) { setSpeed(0, for: key) }
    }

    func resumeAll() {
        for key in Array(displayStates.keys) {
            let remembered = lastRateByDisplay[key] ?? 1
            setSpeed(remembered == 0 ? 1 : remembered, for: key)
        }
    }

    private func schedulePlaybackPolicyApplication(for key: DisplayKey) {
        playbackCommandWorkItems[key]?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.playbackCommandWorkItems[key] = nil
            self.applyPlaybackPolicy(self.currentPlaybackPolicy, for: key)
        }
        playbackCommandWorkItems[key] = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0 / 60.0, execute: work)
    }

    func reapplyVolume() {
        applyPlaybackPolicy(currentPlaybackPolicy)
    }

    func reapplyFrameRate() {
        applyPlaybackPolicy(currentPlaybackPolicy, force: true)
    }

    private func commitPendingAssignmentsForStoppedPolicy() {
        guard !pendingAssignmentProposals.isEmpty else { return }
        // Only the user's latest request per display becomes the desired
        // stopped-state assignment; intermediate requests exist solely because
        // they may have become a visible rollback baseline during handoff.
        let proposals = pendingAssignmentProposals.values.compactMap(\.last)
        pendingAssignmentProposals.removeAll()
        for proposal in proposals {
            let committed = assignmentStateMergingCurrentRuntime(
                proposal.state, for: proposal.key)
            if let previous = displayStates[proposal.key],
               previous.wallpaper.id != committed.wallpaper.id {
                cancelRuntimeSave(for: proposal.key)
                persistRuntime(previous.runtime, for: previous.wallpaper)
            }
            displayStates[proposal.key] = committed
            committedAssignmentIDs[proposal.key] = proposal.id
            lastAppliedPlayback[proposal.key] = nil
        }
        pendingScreenAssignments.removeAll()
        persistStates()
        syncStatusItems()
    }

    func applyPlaybackPolicy(_ action: GSPlayback, force: Bool = false) {
        if action == .stop {
            if !stoppedByPlaybackPolicy {
                cancelAllFailedAssignmentRecoveries()
                commitPendingAssignmentsForStoppedPolicy()
                renderer.stopAll()
                currentByScreen.removeAll()
                stoppedByPlaybackPolicy = true
            }
            lastAppliedPlayback.removeAll()
            return
        }

        if stoppedByPlaybackPolicy {
            stoppedByPlaybackPolicy = false
            lastAppliedPlayback.removeAll()
            for info in DisplayRegistry.shared.connected {
                guard let state = displayStates[info.key] else { continue }
                apply(state, to: info.displayID, key: info.key, reuseActive: true,
                      assignmentID: ensureCommittedAssignmentID(for: info.key))
            }
        }

        for info in DisplayRegistry.shared.connected {
            applyPlaybackPolicy(action, for: info.key, force: force)
        }
    }

    private func applyPlaybackPolicy(_ action: GSPlayback, for key: DisplayKey,
                                     force: Bool = false) {
        guard let displayID = DisplayRegistry.shared.displayID(for: key) else { return }
        if action == .stop {
            renderer.stop(displayID: displayID)
            lastAppliedPlayback[key] = nil
            return
        }

        if let state = displayStates[key] {
            let runtimeState = state.runtime
            let throttledFps = AppDelegate.shared.globalSettingsViewModel
                .throttledFps(base: globalFps)
            let paused = runtimeState.speed == 0 || action == .pause
            let powerState: MiragePowerState = paused ? .pause
                : (throttledFps < globalFps ? .throttle : .run)

            let applied = AppliedPlaybackState(
                paused: paused,
                muted: runtimeState.muted || globalMuted || runtimeState.volume == 0 ||
                    action == .mute,
                volume: runtimeState.volume * masterVolume,
                speed: runtimeState.speed,
                powerState: powerState,
                fps: throttledFps
            )
            let committedAssignmentID = ensureCommittedAssignmentID(for: key)
            if force || applied != lastAppliedPlayback[key] {
                let options = makeRenderOptions(
                    for: state.wallpaper, runtime: runtimeState,
                    assignmentID: committedAssignmentID, playbackAction: action)
                renderer.applyPlaybackOptions(
                    options, assignmentID: committedAssignmentID, onDisplay: displayID)
                lastAppliedPlayback[key] = applied
            }
        }

        for proposal in pendingAssignmentProposals[displayID] ?? []
        where proposal.key == key {
            let proposalState = assignmentStateMergingCurrentRuntime(
                proposal.state, for: proposal.key)
            let options = makeRenderOptions(
                for: proposalState.wallpaper, runtime: proposalState.runtime,
                assignmentID: proposal.id, playbackAction: action)
            renderer.applyPlaybackOptions(
                options, assignmentID: proposal.id, onDisplay: displayID)
        }
    }

    // MARK: 状态栏菜单项文字同步

    private func syncStatusItems() {
        let active = displayStates.filter { DisplayRegistry.shared.displayID(for: $0.key) != nil }
        let muted = !active.isEmpty && active.values.allSatisfy {
            $0.runtime.muted || $0.runtime.volume == 0
        }
        let paused = !active.isEmpty && active.values.allSatisfy { $0.runtime.speed == 0 }
        syncStatusPauseItem(isPaused: paused)
        syncStatusMuteItem(isMuted: muted)
    }

    private func syncStatusPauseItem(isPaused: Bool) {
        guard let menu = AppDelegate.shared.statusItem?.menu else { return }
        let pauseTitle = L("暂停")
        let resumeTitle = L("继续")
        for item in menu.items {
            if isPaused, item.title == pauseTitle {
                item.title = resumeTitle
                item.image = NSImage(systemSymbolName: "play.fill", accessibilityDescription: nil)
                item.action = #selector(AppDelegate.resume)
                item.target = AppDelegate.shared
            } else if !isPaused, item.title == resumeTitle {
                item.title = pauseTitle
                item.image = NSImage(systemSymbolName: "pause.fill", accessibilityDescription: nil)
                item.action = #selector(AppDelegate.pause)
                item.target = AppDelegate.shared
            }
        }
    }

    private func syncStatusMuteItem(isMuted: Bool) {
        guard let menu = AppDelegate.shared.statusItem?.menu else { return }
        let muteTitle = L("静音")
        let unmuteTitle = L("取消静音")
        for (i, item) in menu.items.enumerated() {
            if isMuted, item.title == muteTitle {
                let replacement = NSMenuItem(title: unmuteTitle, systemImage: "speaker.fill",
                                             action: #selector(AppDelegate.unmute), keyEquivalent: "")
                replacement.target = AppDelegate.shared
                menu.items[i] = replacement
            } else if !isMuted, item.title == unmuteTitle {
                let replacement = NSMenuItem(title: muteTitle, systemImage: "speaker.slash.fill",
                                             action: #selector(AppDelegate.mute), keyEquivalent: "")
                replacement.target = AppDelegate.shared
                menu.items[i] = replacement
            }
        }
    }
}
