//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Combine
import Foundation

protocol PlaylistPlayback: AnyObject {
    var displayStatesChanges: CurrentValueSubject<[DisplayKey: DisplayWallpaperState], Never> { get }
    var wallpaperChangeRequests: PassthroughSubject<DisplayKey, Never> { get }
    func state(for key: DisplayKey) -> DisplayWallpaperState?
    func isTrusted(_ wallpaper: WEWallpaper) -> Bool
    func allowsPlaylistAdvance(on key: DisplayKey, manually: Bool, updateOnPause: Bool) -> Bool
    func assign(_ wallpaper: WEWallpaper, to key: DisplayKey, restoreFocus: Bool,
                preservingPlaybackState: Bool, completion: ((Bool) -> Void)?)
}

final class PlaylistRotator {
    enum StartReason { case appLaunch, listChanged, settingsChanged, manualAdvance }

    private struct AdvanceRequest {
        let id = UUID()
        let playlistID: UUID
        let target: PlaylistNavigation.Target
        let direction: PlaylistDirection?
        let manually: Bool
        let excludedIDs: Set<String>
    }

    let screen: Int
    let displayKey: DisplayKey
    private weak var wallpaperViewModel: (any PlaylistPlayback)?
    private weak var manager: PlaylistManager?
    private var timer: DispatchSourceTimer?
    private var videoEndObserver: NSObjectProtocol?
    private var stateSubscription: AnyCancellable?
    private var requestSubscription: AnyCancellable?
    private var navigation = PlaylistNavigation()
    private var pendingRequest: AdvanceRequest?
    private var applyingTargets: [UUID: PlaylistNavigation.Target] = [:]
    private var schedulingGeneration = UUID()
    private var isRunning = false
    private var didHandleLaunch = false
    private var pendingVideoAdvance = false
    private let preparationWorker = LatestValueWorker<String, WEWallpaper?>(
        label: "cn.laobamac.Mirage.playlist.prepare"
    ) { id in
        let wallpaper = WEWallpaper.load(from: URL(fileURLWithPath: id, isDirectory: id.hasSuffix("/")))
        guard wallpaper.presentationIsValid, wallpaper.kind != .unsupported,
              FileManager.default.isReadableFile(atPath: wallpaper.resolvedEntryURL.path) else { return nil }
        return wallpaper
    }

    init(screen: Int, displayKey: DisplayKey, wallpaperViewModel: any PlaylistPlayback, manager: PlaylistManager) {
        self.screen = screen
        self.displayKey = displayKey
        self.wallpaperViewModel = wallpaperViewModel
        self.manager = manager
        navigation.synchronize(with: manager.current(on: screen))
        stateSubscription = wallpaperViewModel.displayStatesChanges.sink { [weak self] states in
            guard let self, let manager = self.manager else { return }
            let id = states[self.displayKey]?.wallpaper.id
            guard !self.applyingTargets.values.contains(where: { $0.wallpaperID == id }) else { return }
            self.navigation.synchronize(with: manager.current(on: self.screen))
            self.navigation.observe(id)
        }
        navigation.observe(wallpaperViewModel.state(for: displayKey)?.wallpaper.id)
        requestSubscription = wallpaperViewModel.wallpaperChangeRequests.sink { [weak self] key in
            guard let self, self.displayKey == key else { return }
            self.stopScheduling()
            self.cancelPendingAdvance()
            self.rebuild(reason: .manualAdvance)
        }
    }

    deinit { stop() }

    func start(reason: StartReason) {
        isRunning = true
        rebuild(reason: reason)
    }

    func stop() {
        isRunning = false
        stopScheduling()
        cancelPendingAdvance()
    }

    private func stopScheduling() {
        schedulingGeneration = UUID()
        timer?.cancel()
        timer = nil
        pendingVideoAdvance = false
        if let observer = videoEndObserver {
            NotificationCenter.default.removeObserver(observer)
            videoEndObserver = nil
        }
    }

    private func cancelPendingAdvance() {
        pendingRequest = nil
        preparationWorker.cancel()
        PlaylistTransitionOverlay.shared.cancel(on: screen)
    }

    func rebuild(reason: StartReason) {
        if Thread.isMainThread {
            guard isRunning else { return }
            rebuildOnMain(reason: reason)
            return
        }
        DispatchQueue.main.async { [weak self] in
            guard let self, self.isRunning else { return }
            self.rebuildOnMain(reason: reason)
        }
    }

    private func rebuildOnMain(reason: StartReason) {
        stopScheduling()
        if reason != .manualAdvance { cancelPendingAdvance() }
        guard isRunning, let manager, let vm = wallpaperViewModel,
              DisplayRegistry.shared.screenIndex(for: displayKey) == screen else { return }
        let playlist = manager.current(on: screen)
        navigation.synchronize(with: playlist)
        if navigation.history.isEmpty { navigation.observe(vm.state(for: displayKey)?.wallpaper.id) }
        guard !playlist.items.isEmpty else { return }

        if reason == .appLaunch, !didHandleLaunch {
            didHandleLaunch = true
            if playlist.settings.introOnStartup || playlist.settings.alwaysBeginFirst,
               let first = playlist.items.first {
                requestFixed(first.wallpaperID, playlist: playlist)
            }
        }

        if playlist.settings.videoSequence { observeVideoEnd() }
        switch playlist.settings.timing {
        case .never, .logon:
            break
        case .timer:
            scheduleTimer(after: playlist.settings.timerIntervalSeconds)
        case .daytime:
            scheduleNextDaytimeAnchor(from: playlist.settings.daytimeAnchors)
        case .dayOfWeek:
            scheduleNextMidnight()
            if reason != .manualAdvance { applyDayOfWeek(playlist: playlist, vm: vm) }
        }
    }

    private func scheduleTimer(after seconds: TimeInterval) {
        let generation = schedulingGeneration
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + seconds, repeating: seconds)
        timer.setEventHandler { [weak self] in
            guard let self, self.schedulingGeneration == generation else { return }
            self.tick()
        }
        timer.resume()
        self.timer = timer
    }

    private func scheduleNextDaytimeAnchor(from anchors: [Int]) {
        let now = Date()
        let calendar = Calendar.current
        let sortedAnchors = anchors.sorted().filter { (0...23).contains($0) }
        guard !sortedAnchors.isEmpty else { return }
        let currentHour = calendar.component(.hour, from: now)
        var target: Date?
        for hour in sortedAnchors where hour > currentHour {
            var comps = calendar.dateComponents([.year, .month, .day], from: now)
            comps.hour = hour; comps.minute = 0; comps.second = 0
            target = calendar.date(from: comps)
            break
        }
        if target == nil, let first = sortedAnchors.first,
           let tomorrow = calendar.date(byAdding: .day, value: 1, to: now) {
            var comps = calendar.dateComponents([.year, .month, .day], from: tomorrow)
            comps.hour = first; comps.minute = 0; comps.second = 0
            target = calendar.date(from: comps)
        }
        guard let fireAt = target else { return }
        let generation = schedulingGeneration
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + max(fireAt.timeIntervalSinceNow, 30))
        timer.setEventHandler { [weak self] in
            guard let self, self.schedulingGeneration == generation else { return }
            self.tick()
            self.timer?.cancel()
            self.scheduleNextDaytimeAnchor(from: anchors)
        }
        timer.resume()
        self.timer = timer
    }

    private func scheduleNextMidnight() {
        let calendar = Calendar.current
        guard let next = calendar.nextDate(after: Date(),
                                           matching: DateComponents(hour: 0, minute: 0, second: 5),
                                           matchingPolicy: .nextTime) else { return }
        let generation = schedulingGeneration
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + max(next.timeIntervalSinceNow, 30))
        timer.setEventHandler { [weak self] in
            guard let self, self.schedulingGeneration == generation,
                  let manager = self.manager, let vm = self.wallpaperViewModel else { return }
            self.applyDayOfWeek(playlist: manager.current(on: self.screen), vm: vm)
            self.timer?.cancel()
            self.scheduleNextMidnight()
        }
        timer.resume()
        self.timer = timer
    }

    private func applyDayOfWeek(playlist: Playlist, vm: any PlaylistPlayback) {
        let today = Calendar.current.component(.weekday, from: Date()) - 1
        let items = playlist.items.prefix(7)
        guard items.indices.contains(today),
              vm.allowsPlaylistAdvance(on: displayKey, manually: false,
                                       updateOnPause: playlist.settings.updateOnPause) else { return }
        requestFixed(items[today].wallpaperID, playlist: playlist)
    }

    private func observeVideoEnd() {
        let generation = schedulingGeneration
        videoEndObserver = NotificationCenter.default.addObserver(
            forName: .rendererVideoDidEnd, object: nil, queue: .main
        ) { [weak self] note in
            guard let self, self.schedulingGeneration == generation,
                  note.userInfo?["screen"] as? Int == self.screen,
                  self.pendingVideoAdvance else { return }
            self.pendingVideoAdvance = false
            self.advance(.next, manually: false)
        }
    }

    private func tick() {
        guard pendingRequest == nil, let vm = wallpaperViewModel, let manager else { return }
        let settings = manager.current(on: screen).settings
        guard vm.allowsPlaylistAdvance(on: displayKey, manually: false,
                                       updateOnPause: settings.updateOnPause) else { return }
        if settings.videoSequence, vm.state(for: displayKey)?.wallpaper.kind == .video {
            pendingVideoAdvance = true
            return
        }
        advance(.next, manually: false)
    }

    func canAdvance(_ direction: PlaylistDirection, library: [WEWallpaper]) -> Bool {
        guard isRunning, let manager, let vm = wallpaperViewModel,
              DisplayRegistry.shared.screenIndex(for: displayKey) == screen else { return false }
        let playlist = manager.current(on: screen)
        guard vm.allowsPlaylistAdvance(on: displayKey, manually: true,
                                       updateOnPause: playlist.settings.updateOnPause) else { return false }
        navigation.synchronize(with: playlist)
        let available = playableIDs(in: playlist, library: library, vm: vm)
        return !navigation.candidates(for: direction, in: playlist, availableIDs: available,
                                      currentID: vm.state(for: displayKey)?.wallpaper.id,
                                      pending: pendingRequest?.target).isEmpty
    }

    func advanceManually(_ direction: PlaylistDirection, library: [WEWallpaper]) {
        guard canAdvance(direction, library: library) else { return }
        rebuildOnMain(reason: .manualAdvance)
        advance(direction, manually: true, library: library)
    }

    private func playableIDs(in playlist: Playlist, library: [WEWallpaper], vm: any PlaylistPlayback) -> Set<String> {
        let ids = Set(playlist.items.map(\.wallpaperID))
        return Set(library.lazy.filter {
            ids.contains($0.id) && $0.presentationIsValid && $0.kind != .unsupported &&
                ($0.kind != .web || vm.isTrusted($0))
        }.map(\.id))
    }

    private func advance(_ direction: PlaylistDirection, manually: Bool,
                         library: [WEWallpaper]? = nil, excludedIDs: Set<String> = []) {
        guard isRunning, let manager, let vm = wallpaperViewModel,
              DisplayRegistry.shared.screenIndex(for: displayKey) == screen else { return }
        let playlist = manager.current(on: screen)
        guard vm.allowsPlaylistAdvance(on: displayKey, manually: manually,
                                       updateOnPause: playlist.settings.updateOnPause) else {
            cancelPendingAdvance()
            return
        }
        navigation.synchronize(with: playlist)
        let available = library.map { playableIDs(in: playlist, library: $0, vm: vm) }
            ?? Set(playlist.items.map(\.wallpaperID))
        let candidates = navigation.candidates(
            for: direction, in: playlist, availableIDs: available.subtracting(excludedIDs),
            currentID: vm.state(for: displayKey)?.wallpaper.id, pending: pendingRequest?.target)
        let target = playlist.settings.order == .random && direction == .next && candidates.first?.historyIndex == nil
            ? candidates.randomElement() : candidates.first
        guard let target else {
            cancelPendingAdvance()
            return
        }
        begin(AdvanceRequest(playlistID: playlist.id, target: target, direction: direction,
                             manually: manually, excludedIDs: excludedIDs))
    }

    private func requestFixed(_ id: String, playlist: Playlist) {
        guard let vm = wallpaperViewModel,
              vm.state(for: displayKey)?.wallpaper.id != id,
              vm.allowsPlaylistAdvance(on: displayKey, manually: false,
                                       updateOnPause: playlist.settings.updateOnPause) else { return }
        begin(AdvanceRequest(playlistID: playlist.id, target: .init(wallpaperID: id),
                             direction: nil, manually: false, excludedIDs: []))
    }

    private func begin(_ request: AdvanceRequest) {
        pendingRequest = request
        PlaylistTransitionOverlay.shared.cancel(on: screen)
        preparationWorker.submit(request.target.wallpaperID) { [weak self] wallpaper in
            guard let self, self.pendingRequest?.id == request.id,
                  let vm = self.wallpaperViewModel, let manager = self.manager else { return }
            let playlist = manager.current(on: self.screen)
            guard playlist.id == request.playlistID,
                  playlist.items.contains(where: { $0.wallpaperID == request.target.wallpaperID }),
                  DisplayRegistry.shared.screenIndex(for: self.displayKey) == self.screen,
                  vm.allowsPlaylistAdvance(on: self.displayKey, manually: request.manually,
                                           updateOnPause: playlist.settings.updateOnPause) else {
                self.cancelPendingAdvance()
                return
            }
            guard let wallpaper, wallpaper.kind != .web || vm.isTrusted(wallpaper) else {
                self.retry(request)
                return
            }
            let settings = playlist.settings
            PlaylistTransitionOverlay.shared.present(
                on: self.screen, duration: settings.transitionSeconds, kind: settings.transition
            ) { [weak self, weak vm] in
                guard let self, let vm, self.pendingRequest?.id == request.id else { return }
                guard DisplayRegistry.shared.screenIndex(for: self.displayKey) == self.screen,
                      vm.allowsPlaylistAdvance(on: self.displayKey, manually: request.manually,
                                               updateOnPause: settings.updateOnPause) else {
                    self.cancelPendingAdvance()
                    return
                }
                self.applyingTargets[request.id] = request.target
                vm.assign(wallpaper, to: self.displayKey, restoreFocus: false,
                          preservingPlaybackState: true) { [weak self] success in
                    guard let self else { return }
                    self.applyingTargets[request.id] = nil
                    if success, self.manager?.current(on: self.screen).id == request.playlistID {
                        self.navigation.commit(request.target)
                    }
                    guard self.pendingRequest?.id == request.id else { return }
                    if success {
                        self.pendingRequest = nil
                        if request.manually { self.rebuildOnMain(reason: .manualAdvance) }
                    } else {
                        self.retry(request)
                    }
                }
            }
        }
    }

    private func retry(_ request: AdvanceRequest) {
        guard pendingRequest?.id == request.id else { return }
        guard let direction = request.direction else {
            cancelPendingAdvance()
            return
        }
        var excluded = request.excludedIDs
        excluded.insert(request.target.wallpaperID)
        if let currentID = wallpaperViewModel?.state(for: displayKey)?.wallpaper.id {
            excluded.insert(currentID)
        }
        advance(direction, manually: request.manually, excludedIDs: excluded)
    }
}
