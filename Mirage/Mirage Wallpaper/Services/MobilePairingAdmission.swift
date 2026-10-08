import Foundation

/// Accessed only on MobilePairingService's serial queue. Windows use monotonic time.
struct MobilePairingAdmission {
    private struct Window {
        var began: TimeInterval
        var connections = 0
        var failures = 0
    }
    private var peers: [String: Window] = [:]
    private var global = Window(began: 0)

    private mutating func refresh(_ peer: String, now: TimeInterval) {
        peers = peers.filter { now - $0.value.began < 60 }
        if now - global.began >= 60 { global = Window(began: now) }
        if peers[peer] == nil {
            if peers.count >= 128, let oldest = peers.min(by: { $0.value.began < $1.value.began }) {
                peers[oldest.key] = nil
            }
            peers[peer] = Window(began: now)
        }
    }

    mutating func admit(_ peer: String, pending: Int, total: Int, now: TimeInterval) -> Bool {
        refresh(peer, now: now)
        guard pending < 8, total < 64, global.connections < 60, global.failures < 30,
              let window = peers[peer], window.connections < 12, window.failures < 5 else { return false }
        peers[peer]?.connections += 1
        global.connections += 1
        return true
    }

    mutating func canAttempt(_ peer: String, now: TimeInterval) -> Bool {
        refresh(peer, now: now)
        return global.failures < 30 && (peers[peer]?.failures ?? 0) < 5
    }

    mutating func failed(_ peer: String, now: TimeInterval) {
        refresh(peer, now: now)
        peers[peer]?.failures += 1
        global.failures += 1
    }

    static func accepts(version: Int?, submittedPIN: String, currentPIN: String?,
                        pairingActive: Bool, savedPIN: String?) -> Bool {
        version == 4 && ((pairingActive && submittedPIN == currentPIN) || submittedPIN == savedPIN)
    }
}
