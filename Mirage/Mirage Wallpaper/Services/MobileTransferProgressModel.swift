//
//  Mirage Wallpaper
//
//  Progress state for native Wallpaper Engine Android transfers.
//

import Combine
import Foundation

final class MobileTransferProgressModel: ObservableObject {
    static let shared = MobileTransferProgressModel()

    enum Destination: Equatable {
        case device(String)
        case file
    }

    enum Phase: Equatable {
        case preparing
        case converting
        case waitingForDevice
        case uploading
        case completed
        case failed(String)
    }

    struct Job: Identifiable, Equatable {
        let id: UUID
        let wallpaperTitle: String
        let destination: Destination
        var phase: Phase
        var progress: Double
    }

    @Published private(set) var jobs: [Job] = []

    private var removalTasks: [UUID: DispatchWorkItem] = [:]

    private init() {}

    @discardableResult
    func start(
        wallpaperTitle: String,
        deviceName: String,
        initialPhase: Phase = .preparing
    ) -> UUID {
        start(
            wallpaperTitle: wallpaperTitle,
            destination: .device(deviceName),
            initialPhase: initialPhase
        )
    }

    @discardableResult
    func startExport(
        wallpaperTitle: String,
        initialPhase: Phase
    ) -> UUID {
        start(
            wallpaperTitle: wallpaperTitle,
            destination: .file,
            initialPhase: initialPhase
        )
    }

    private func start(
        wallpaperTitle: String,
        destination: Destination,
        initialPhase: Phase
    ) -> UUID {
        let id = UUID()
        let append: () -> Void = { [weak self] in
            guard let self else { return }
            self.removalTasks[id]?.cancel()
            self.jobs.append(
                Job(
                    id: id,
                    wallpaperTitle: wallpaperTitle,
                    destination: destination,
                    phase: initialPhase,
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
        updateInitialProgress(id: id, phase: .preparing, fraction: fraction)
    }

    func updateConversion(id: UUID, fraction: Double) {
        updateInitialProgress(id: id, phase: .converting, fraction: fraction)
    }

    func waitForDevice(id: UUID) {
        update(id: id, phase: .waitingForDevice, progress: 0.5)
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
            self.scheduleRemoval(id: id, after: 3)
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

    private func updateInitialProgress(id: UUID, phase: Phase, fraction: Double) {
        let clamped = min(max(fraction, 0), 1)
        updateOnMain { [weak self] in
            guard let self,
                  let index = self.jobs.firstIndex(where: { $0.id == id }) else { return }
            guard Self.phaseRank(phase) >= Self.phaseRank(self.jobs[index].phase) else { return }
            let share: Double
            switch self.jobs[index].destination {
            case .device:
                share = 0.5
            case .file:
                share = 1
            }
            self.jobs[index].phase = phase
            self.jobs[index].progress = max(self.jobs[index].progress, clamped * share)
        }
    }

    private func remove(id: UUID) {
        removalTasks[id]?.cancel()
        removalTasks[id] = nil
        jobs.removeAll { $0.id == id }
    }

    private func scheduleRemoval(id: UUID, after delay: TimeInterval) {
        removalTasks[id]?.cancel()
        let task = DispatchWorkItem { [weak self] in
            self?.remove(id: id)
        }
        removalTasks[id] = task
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: task)
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
        case .preparing, .converting: return 0
        case .waitingForDevice: return 1
        case .uploading: return 2
        case .completed, .failed: return 3
        }
    }
}
