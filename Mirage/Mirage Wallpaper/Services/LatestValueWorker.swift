//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

final class LatestValueWorker<Input, Output>: @unchecked Sendable {
    private let queue: DispatchQueue
    private let process: (Input) -> Output
    private let lock = NSLock()
    private var generation: UInt64 = 0
    private var running = false
    private var pending: (Input, UInt64, (Output) -> Void)?

    init(label: String, process: @escaping (Input) -> Output) {
        queue = DispatchQueue(label: label, qos: .userInitiated)
        self.process = process
    }

    func submit(_ input: Input, completion: @escaping (Output) -> Void) {
        lock.lock()
        generation &+= 1
        pending = (input, generation, completion)
        let shouldStart = !running
        running = true
        lock.unlock()
        if shouldStart {
            queue.async { [self] in drain() }
        }
    }

    func cancel() {
        lock.lock()
        generation &+= 1
        pending = nil
        lock.unlock()
    }

    private func drain() {
        while true {
            lock.lock()
            guard let (input, token, completion) = pending else {
                running = false
                lock.unlock()
                return
            }
            pending = nil
            lock.unlock()
            let output = autoreleasepool { process(input) }
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.lock.lock()
                let isCurrent = self.generation == token
                self.lock.unlock()
                if isCurrent { completion(output) }
            }
        }
    }
}
