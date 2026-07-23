//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import Foundation

final class PlaylistRotator {
    enum StartReason { case appLaunch, listChanged, settingsChanged, manualAdvance }

    private let screen: Int
    private weak var wallpaperViewModel: WallpaperViewModel?
    private weak var manager: PlaylistManager?

    private var timer: DispatchSourceTimer?
    private var videoEndObserver: NSObjectProtocol?
    private var lastPlayedID: String?
    private var didHandleLaunch = false
    private var pendingVideoAdvance = false

    init(screen: Int, wallpaperViewModel: WallpaperViewModel, manager: PlaylistManager) {
        self.screen = screen
        self.wallpaperViewModel = wallpaperViewModel
        self.manager = manager
    }

    deinit { stop() }

    func start(reason: StartReason) {
        rebuild(reason: reason)
    }

    func stop() {
        timer?.cancel()
        timer = nil
        if let observer = videoEndObserver {
            NotificationCenter.default.removeObserver(observer)
            videoEndObserver = nil
        }
    }

    // MARK: Scheduling

    func rebuild(reason: StartReason) {
        DispatchQueue.main.async { [weak self] in
            self?.rebuildOnMain(reason: reason)
        }
    }

    private func rebuildOnMain(reason: StartReason) {
        stop()
        pendingVideoAdvance = false
        guard let manager, let vm = wallpaperViewModel else { return }
        let playlist = manager.current(on: screen)
        guard playlist.items.count > 0 else { return }

        if reason == .appLaunch || playlist.settings.alwaysBeginFirst || playlist.settings.introOnStartup {
            applyLaunchAnchor(playlist: playlist, vm: vm, reason: reason)
        }

        if playlist.settings.videoSequence {
            observeVideoEnd()
        }

        switch playlist.settings.timing {
        case .never:
            return
        case .logon:
            return
        case .timer:
            scheduleTimer(after: playlist.settings.timerIntervalSeconds)
        case .daytime:
            scheduleNextDaytimeAnchor(from: playlist.settings.daytimeAnchors)
        case .dayOfWeek:
            scheduleNextMidnight()
            applyDayOfWeek(playlist: playlist, vm: vm)
        }
    }

    private func applyLaunchAnchor(playlist: Playlist, vm: WallpaperViewModel, reason: StartReason) {
        guard reason == .appLaunch, !didHandleLaunch else { return }
        didHandleLaunch = true
        guard let target = firstItemWallpaper(playlist: playlist) else { return }
        if playlist.settings.introOnStartup || playlist.settings.alwaysBeginFirst {
            apply(target, on: vm)
        }
    }

    private func firstItemWallpaper(playlist: Playlist) -> WEWallpaper? {
        guard let first = playlist.items.first else { return nil }
        let library = WallpaperLibrary.shared.loadAll()
        return library.first(where: { $0.id == first.wallpaperID })
    }

    private func scheduleTimer(after seconds: TimeInterval) {
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + seconds, repeating: seconds)
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
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
        let delay = max(fireAt.timeIntervalSinceNow, 30)
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + delay)
        t.setEventHandler { [weak self] in
            self?.tick()
            self?.rebuildOnMain(reason: .listChanged)
        }
        t.resume()
        timer = t
    }

    private func scheduleNextMidnight() {
        let calendar = Calendar.current
        guard let next = calendar.nextDate(after: Date(),
                                           matching: DateComponents(hour: 0, minute: 0, second: 5),
                                           matchingPolicy: .nextTime) else { return }
        let delay = max(next.timeIntervalSinceNow, 30)
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + delay)
        t.setEventHandler { [weak self] in
            self?.rebuildOnMain(reason: .listChanged)
        }
        t.resume()
        timer = t
    }

    private func applyDayOfWeek(playlist: Playlist, vm: WallpaperViewModel) {
        let today = Calendar.current.component(.weekday, from: Date()) - 1
        let library = WallpaperLibrary.shared.loadAll()
        let items = playlist.items.prefix(7)
        guard today >= 0, today < items.count else { return }
        let wallpaperID = items[items.index(items.startIndex, offsetBy: today)].wallpaperID
        if let wallpaper = library.first(where: { $0.id == wallpaperID }) {
            apply(wallpaper, on: vm)
        }
    }

    private func observeVideoEnd() {
        let name = Notification.Name.rendererVideoDidEnd
        videoEndObserver = NotificationCenter.default.addObserver(
            forName: name, object: nil, queue: .main
        ) { [weak self] note in
            guard let self else { return }
            guard let s = note.userInfo?["screen"] as? Int, s == self.screen else { return }
            guard self.pendingVideoAdvance else { return }
            self.pendingVideoAdvance = false
            self.advanceNow()
        }
    }

    // MARK: Advance

    // Timer / anchor fire. With videoSequence on and a video currently showing,
    // defer the switch until the video finishes its current loop; otherwise
    // advance right away.
    private func tick() {
        guard let vm = wallpaperViewModel else { return }
        let settings = manager?.current(on: screen).settings ?? PlaylistSettings.default()
        if settings.videoSequence, vm.currentByScreen[screen]?.kind == .video {
            pendingVideoAdvance = true
            return
        }
        advanceNow()
    }

    private func advanceNow() {
        guard let manager, let vm = wallpaperViewModel else { return }
        guard shouldAdvance(vm: vm) else { return }
        let playlist = manager.current(on: screen)
        guard let next = pickNext(from: playlist, vm: vm) else { return }
        apply(next, on: vm)
    }

    private func shouldAdvance(vm: WallpaperViewModel) -> Bool {
        let policy = AppDelegate.shared.globalSettingsViewModel.effectivePlaybackAction
        if policy == .stop { return false }
        let manager = manager
        let settings = manager?.current(on: screen).settings ?? PlaylistSettings.default()
        if policy == .pause && !settings.updateOnPause { return false }
        return true
    }

    private func pickNext(from playlist: Playlist, vm: WallpaperViewModel) -> WEWallpaper? {
        let library = WallpaperLibrary.shared.loadAll()
        let items = playlist.items
        guard !items.isEmpty else { return nil }
        let ids = items.map(\.wallpaperID)
        let byID = Dictionary(uniqueKeysWithValues: library.map { ($0.id, $0) })
        let resolved = ids.compactMap { byID[$0] }
        guard !resolved.isEmpty else { return nil }
        let currentID = vm.currentByScreen[screen]?.id ?? lastPlayedID
        switch playlist.settings.order {
        case .sorted:
            if let currentID, let idx = ids.firstIndex(of: currentID) {
                let target = ids[(idx + 1) % ids.count]
                return byID[target] ?? resolved.first
            }
            return resolved.first
        case .random:
            guard resolved.count > 1 else { return resolved.first }
            var pool = resolved
            if let currentID { pool.removeAll { $0.id == currentID } }
            return pool.randomElement() ?? resolved.first
        }
    }

    private func apply(_ wallpaper: WEWallpaper, on vm: WallpaperViewModel) {
        lastPlayedID = wallpaper.id
        let settings = manager?.current(on: screen).settings ?? PlaylistSettings.default()
        let duration = settings.transition == .disabled ? 0 : settings.transitionSeconds
        PlaylistTransitionOverlay.shared.present(
            on: screen, duration: duration, kind: settings.transition
        ) {
            if self.screen == 0 {
                vm.currentWallpaper = wallpaper
            } else {
                vm.applyOnScreen(wallpaper, screen: self.screen)
            }
        }
    }
}
