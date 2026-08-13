//
//  Mirage Wallpaper
//
//  Progress state for native Wallpaper Engine Android transfers.
//

import Combine
import Foundation

final class MobileTransferProgressModel: ObservableObject {
    static let shared = MobileTransferProgressModel()

    enum Phase: Equatable {
        case preparing
        case uploading
        case completed
        case failed(String)
    }

    struct Job: Identifiable, Equatable {
        let id: UUID
        let wallpaperTitle: String
        let deviceName: String
        var phase: Phase
        var progress: Double
    }

    @Published private(set) var jobs: [Job] = []

    private var removalTasks: [UUID: DispatchWorkItem] = [:]

    private init() {}

    @discardableResult
    func start(wallpaperTitle: String, deviceName: String) -> UUID {
        let id = UUID()
        let append: () -> Void = { [weak self] in
            guard let self else { return }
            self.removalTasks[id]?.cancel()
            self.jobs.append(
                Job(
                    id: id,
                    wallpaperTitle: wallpaperTitle,
                    deviceName: deviceName,
                    phase: .preparing,
                    progress: 0
                )
            )
        }
        if Thread.isMainThread {
            append()
        } else {
            DispatchQueue.main.sync(execute: append)
        }
        return id
    }

    func updatePreparation(id: UUID, completedBytes: UInt64, totalBytes: UInt64) {
        let fraction = Self.fraction(completedBytes: completedBytes, totalBytes: totalBytes)
        update(id: id, phase: .preparing, progress: fraction * 0.5)
    }

    func beginUploading(id: UUID) {
        update(id: id, phase: .uploading, progress: 0.5)
    }

    func updateUpload(id: UUID, completedBytes: UInt64, totalBytes: UInt64) {
        let fraction = Self.fraction(completedBytes: completedBytes, totalBytes: totalBytes)
        update(id: id, phase: .uploading, progress: 0.5 + fraction * 0.5)
    }

    func complete(id: UUID) {
        updateOnMain { [weak self] in
            guard let self,
                  let index = self.jobs.firstIndex(where: { $0.id == id }) else { return }
            self.jobs[index].phase = .completed
            self.jobs[index].progress = 1
            self.scheduleRemoval(id: id, after: 4)
        }
    }

    func fail(id: UUID, message: String) {
        updateOnMain { [weak self] in
            guard let self,
                  let index = self.jobs.firstIndex(where: { $0.id == id }) else { return }
            self.removalTasks[id]?.cancel()
            self.removalTasks[id] = nil
            self.jobs[index].phase = .failed(message)
        }
    }

    func dismiss(id: UUID) {
        updateOnMain { [weak self] in
            self?.remove(id: id)
        }
    }

    private func update(id: UUID, phase: Phase, progress: Double) {
        let clamped = min(max(progress, 0), 1)
        updateOnMain { [weak self] in
            guard let self,
                  let index = self.jobs.firstIndex(where: { $0.id == id }) else { return }
            guard Self.phaseRank(phase) >= Self.phaseRank(self.jobs[index].phase) else { return }
            self.jobs[index].phase = phase
            self.jobs[index].progress = max(self.jobs[index].progress, clamped)
        }
    }

    private func scheduleRemoval(id: UUID, after delay: TimeInterval) {
        removalTasks[id]?.cancel()
        let task = DispatchWorkItem { [weak self] in
            self?.remove(id: id)
        }
        removalTasks[id] = task
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: task)
    }

    private func remove(id: UUID) {
        removalTasks[id]?.cancel()
        removalTasks[id] = nil
        jobs.removeAll { $0.id == id }
    }

    private func updateOnMain(_ action: @escaping () -> Void) {
        if Thread.isMainThread {
            action()
        } else {
            DispatchQueue.main.async(execute: action)
        }
    }

    private static func fraction(completedBytes: UInt64, totalBytes: UInt64) -> Double {
        guard totalBytes > 0 else { return 1 }
        return min(Double(completedBytes) / Double(totalBytes), 1)
    }

    private static func phaseRank(_ phase: Phase) -> Int {
        switch phase {
        case .preparing: return 0
        case .uploading: return 1
        case .completed, .failed: return 2
        }
    }
}
