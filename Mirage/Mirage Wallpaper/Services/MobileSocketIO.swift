//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Darwin
import Foundation

/// Bounded writes without changing the blocking mode used by the connection's reader.
enum MobileSocketIO {
    static func writeAll(_ data: Data, to socket: Int32, idleTimeout: TimeInterval = 30) throws {
        guard idleTimeout.isFinite, idleTimeout > 0, idleTimeout <= Double(Int32.max) else {
            throw POSIXError(.EINVAL)
        }
        var deadline = ProcessInfo.processInfo.systemUptime + idleTimeout
        var sent = 0
        try data.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress else { return }
            while sent < data.count {
                let remaining = deadline - ProcessInfo.processInfo.systemUptime
                guard remaining > 0 else { throw POSIXError(.ETIMEDOUT) }
                let microseconds = Int64(ceil(remaining * 1_000_000))
                var timeout = timeval(tv_sec: Int(microseconds / 1_000_000),
                                      tv_usec: Int32(microseconds % 1_000_000))
                guard setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                                 socklen_t(MemoryLayout<timeval>.size)) == 0 else {
                    throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
                }
                let amount = Darwin.send(socket, base.advanced(by: sent), data.count - sent, 0)
                if amount > 0 {
                    sent += amount
                    // Large transfers may take minutes; only a stalled write should time out.
                    deadline = ProcessInfo.processInfo.systemUptime + idleTimeout
                    continue
                }
                if amount == 0 { throw POSIXError(.EPIPE) }
                let code = errno
                if code == EINTR { continue }
                if code == EAGAIN || code == EWOULDBLOCK { throw POSIXError(.ETIMEDOUT) }
                throw POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
            }
        }
    }
}
