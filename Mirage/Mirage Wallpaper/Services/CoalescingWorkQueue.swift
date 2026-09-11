//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

final class CoalescingWorkQueue: @unchecked Sendable {
    private let queue: DispatchQueue
    private let lock = NSLock()
    private var order: [String] = []
    private var pending: [String: () -> Void] = [:]
    private var running = false

    init(label: String) {
        queue = DispatchQueue(label: label, qos: .utility)
    }

    func submit(key: String, _ work: @escaping () -> Void) {
        lock.lock()
        if pending[key] == nil { order.append(key) }
        pending[key] = work
        let shouldStart = !running
        running = true
        if shouldStart { queue.async { [self] in drain() } }
        lock.unlock()
    }

    func flush() {
        queue.sync {}
    }

    private func drain() {
        while true {
            lock.lock()
            guard !order.isEmpty else {
                running = false
                lock.unlock()
                return
            }
            let key = order.removeFirst()
            let work = pending.removeValue(forKey: key)
            lock.unlock()
            autoreleasepool { work?() }
        }
    }
}
