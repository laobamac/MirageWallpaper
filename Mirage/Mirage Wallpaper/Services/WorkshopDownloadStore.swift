//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation
import Observation

@MainActor
@Observable
final class WorkshopDownloadStatus: Identifiable, Equatable {
    let id: String
    private(set) var task: DownloadTask?

    var state: DownloadState? {
        task?.state
    }

    init(workshopID: String, task: DownloadTask? = nil) {
        id = workshopID
        self.task = task
    }

    nonisolated static func == (
        lhs: WorkshopDownloadStatus,
        rhs: WorkshopDownloadStatus
    ) -> Bool {
        lhs === rhs
    }

    fileprivate func replaceTask(_ task: DownloadTask?) {
        self.task = task
    }

    fileprivate func updateTask(_ update: (inout DownloadTask) -> Void) {
        guard var task else { return }
        update(&task)
        self.task = task
    }
}

@MainActor
@Observable
final class WorkshopDownloadStore {
    private(set) var queue: [WorkshopDownloadStatus] = []
    private(set) var activeDownloadCount = 0
    private(set) var completedDownloadCount = 0
    private(set) var clearableDownloadCount = 0

    @ObservationIgnored
    private let statuses = NSMapTable<NSString, WorkshopDownloadStatus>(
        keyOptions: .strongMemory,
        valueOptions: .weakMemory
    )
    @ObservationIgnored private var cancelledAttemptIDs: Set<String> = []
    @ObservationIgnored private var canProcessDownloads: () -> Bool = { false }
    @ObservationIgnored private var isInstalled: (String) -> Bool = { _ in false }
    @ObservationIgnored private var onServiceStateChange: (DownloadState) -> Void = { _ in }
    @ObservationIgnored private var onCompletedDownload: (String, DownloadPurpose) -> Void = { _, _ in }

    func configure(
        canProcessDownloads: @escaping () -> Bool,
        isInstalled: @escaping (String) -> Bool,
        onServiceStateChange: @escaping (DownloadState) -> Void,
        onCompletedDownload: @escaping (String, DownloadPurpose) -> Void
    ) {
        self.canProcessDownloads = canProcessDownloads
        self.isInstalled = isInstalled
        self.onServiceStateChange = onServiceStateChange
        self.onCompletedDownload = onCompletedDownload
    }

    /// Returns one stable observable object per visible or queued Workshop item.
    /// The weak registry avoids retaining statuses after both the card and queue
    /// stop using them.
    func status(for workshopID: String) -> WorkshopDownloadStatus {
        let key = workshopID as NSString
        if let status = statuses.object(forKey: key) {
            return status
        }
        let status = WorkshopDownloadStatus(workshopID: workshopID)
        statuses.setObject(status, forKey: key)
        return status
    }

    func task(for workshopID: String) -> DownloadTask? {
        statuses.object(forKey: workshopID as NSString)?.task
    }

    func state(for workshopID: String) -> DownloadState? {
        status(for: workshopID).state
    }

    var pendingWorkshopIDs: Set<String> {
        Set(queue.compactMap { status in
            guard let task = status.task else { return nil }
            switch task.state {
            case .queued, .resolving, .downloading, .validating:
                return task.id
            case .completed, .failed:
                return nil
            }
        })
    }

    func download(_ item: WorkshopItem, purpose: DownloadPurpose = .wallpaper) {
        guard !isInstalled(item.publishedFileId) else { return }

        let status = status(for: item.publishedFileId)
        if let existingTask = status.task {
            switch existingTask.state {
            case .failed, .completed:
                removeStatusFromQueue(status)
            case .queued, .resolving, .downloading, .validating:
                if purpose == .presetDependency {
                    status.updateTask { $0.purpose = purpose }
                }
                return
            }
        }

        status.replaceTask(DownloadTask(
            workshopItem: item,
            attemptID: nil,
            state: .queued,
            startedAt: nil,
            completedAt: nil,
            purpose: purpose
        ))
        queue.append(status)
        recomputeCounts()
        processQueue()
    }

    func cancel(_ item: WorkshopItem) {
        guard let status = statuses.object(forKey: item.publishedFileId as NSString),
              let task = status.task else { return }

        if case .queued = task.state {
            removeStatusFromQueue(status)
            processQueue()
            return
        }
        guard let attemptID = task.attemptID else { return }
        cancelledAttemptIDs.insert(attemptID)
        SteamServiceManager.shared.cancelDownload(taskId: attemptID)
    }

    func cancelForUnsubscribe(workshopID: String) {
        guard let status = statuses.object(forKey: workshopID as NSString),
              let task = status.task else { return }
        if let attemptID = task.attemptID {
            SteamServiceManager.shared.cancelDownload(taskId: attemptID)
        }
        removeStatusFromQueue(status)
        processQueue()
    }

    func retry(_ task: DownloadTask) {
        if let status = statuses.object(forKey: task.id as NSString) {
            removeStatusFromQueue(status)
        }
        download(task.workshopItem, purpose: task.purpose)
    }

    func clearCompleted() {
        removeStatuses { task in
            if case .completed = task.state { return true }
            if case .failed = task.state { return true }
            return false
        }
    }

    func removeCompletedDownload(for workshopID: String) {
        removeStatuses { task in
            guard task.id == workshopID else { return false }
            if case .completed = task.state { return true }
            return false
        }
    }

    func reconcileCompletedDownloads(installedWorkshopIDs: Set<String>) {
        removeStatuses { task in
            guard !installedWorkshopIDs.contains(task.id) else { return false }
            if case .completed = task.state { return true }
            return false
        }
    }

    func processQueue() {
        guard canProcessDownloads() else { return }
        let maxConcurrent = 3
        var currentActive = activeDownloadCount

        while currentActive < maxConcurrent,
              let status = queue.first(where: {
                  guard let task = $0.task else { return false }
                  if case .queued = task.state { return true }
                  return false
              }),
              let task = status.task {
            let workshopID = task.workshopItem.publishedFileId
            let attemptID = UUID().uuidString
            status.updateTask {
                $0.attemptID = attemptID
                $0.state = .resolving
                $0.startedAt = Date()
            }
            currentActive += 1
            recomputeCounts()

            SteamServiceManager.shared.downloadItem(
                workshopId: workshopID,
                taskId: attemptID
            ) { [weak self] state in
                Task { @MainActor [weak self] in
                    self?.handleDownloadState(
                        state,
                        workshopID: workshopID,
                        attemptID: attemptID
                    )
                }
            }
        }
    }

    private func handleDownloadState(
        _ state: DownloadState,
        workshopID: String,
        attemptID: String
    ) {
        guard let status = statuses.object(forKey: workshopID as NSString),
              let task = status.task,
              task.attemptID == attemptID else { return }

        status.updateTask { $0.state = state }
        recomputeCounts()

        if cancelledAttemptIDs.contains(attemptID), case .failed = state {
            cancelledAttemptIDs.remove(attemptID)
            removeStatusFromQueue(status)
            processQueue()
            return
        }

        switch state {
        case .completed:
            cancelledAttemptIDs.remove(attemptID)
            if let directory = SteamServiceManager.shared.downloadedItemDirectory(
                workshopId: workshopID
            ) {
                WallpaperLibrary.shared.recordAdded(at: directory, workshopID: workshopID)
            }
            let purpose = task.purpose
            status.updateTask { $0.completedAt = Date() }
            onServiceStateChange(state)
            processQueue()
            NotificationCenter.default.post(name: .workshopItemDownloaded, object: workshopID)
            onCompletedDownload(workshopID, purpose)
        case .failed:
            onServiceStateChange(state)
            if SteamServiceManager.shared.isLoggedIn {
                processQueue()
            }
        case .resolving:
            onServiceStateChange(state)
        case .queued, .downloading, .validating:
            break
        }
    }

    private func removeStatusFromQueue(_ status: WorkshopDownloadStatus) {
        queue.removeAll { $0 === status }
        status.replaceTask(nil)
        recomputeCounts()
    }

    private func removeStatuses(where shouldRemove: (DownloadTask) -> Bool) {
        var removed: [WorkshopDownloadStatus] = []
        queue.removeAll { status in
            guard let task = status.task, shouldRemove(task) else { return false }
            removed.append(status)
            return true
        }
        for status in removed {
            status.replaceTask(nil)
        }
        recomputeCounts()
    }

    private func recomputeCounts() {
        var active = 0
        var completed = 0
        var clearable = 0
        for status in queue {
            guard let state = status.state else { continue }
            switch state {
            case .resolving, .downloading, .validating:
                active += 1
            case .completed:
                completed += 1
                clearable += 1
            case .failed:
                clearable += 1
            case .queued:
                break
            }
        }
        if activeDownloadCount != active {
            activeDownloadCount = active
        }
        if completedDownloadCount != completed {
            completedDownloadCount = completed
        }
        if clearableDownloadCount != clearable {
            clearableDownloadCount = clearable
        }
    }
}
