//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Combine
import SwiftUI

/// Live state of every video wallpaper currently being rewritten to H.264.
/// Keyed by screen so two displays converting at once each get their own row.
final class VideoTranscodeProgressModel: ObservableObject {
    static let shared = VideoTranscodeProgressModel()

    struct Job: Identifiable, Equatable {
        let id: Int
        let title: String
        var progress: Double
    }

    @Published private(set) var jobs: [Job] = []

    private init() {}

    func update(screen: Int, title: String, progress: Double) {
        let clamped = min(max(progress, 0), 1)
        if let index = jobs.firstIndex(where: { $0.id == screen }) {
            // A different wallpaper on the same screen is a new conversion, so
            // the row is replaced rather than merged — otherwise it would keep
            // the previous title and, through the max() below, its progress too.
            if jobs[index].title != title {
                jobs[index] = Job(id: screen, title: title, progress: clamped)
                return
            }
            // Progress is reported per decoded frame and the bar only ever moves
            // forward, so a late callback from a slower queue cannot rewind it.
            jobs[index].progress = max(jobs[index].progress, clamped)
        } else {
            jobs.append(Job(id: screen, title: title, progress: clamped))
            jobs.sort { $0.id < $1.id }
        }
    }

    func finish(screen: Int) {
        jobs.removeAll { $0.id == screen }
    }

    func finishAll() {
        jobs.removeAll()
    }
}
