import Darwin
import Foundation

@main
struct MobilePairingRegression {
    static func require(_ value: @autoclosure () -> Bool, _ message: String) {
        precondition(value(), message)
    }
    static func main() throws {
        var policy = MobilePairingAdmission()
        require(!policy.admit("a", pending: 8, total: 8, now: 100), "pending limit")
        require(!policy.admit("a", pending: 0, total: 64, now: 100), "total limit")
        require(policy.admit("a", pending: 0, total: 0, now: 100), "normal connection")
        for _ in 0..<5 { policy.failed("a", now: 101) }
        require(!policy.canAttempt("a", now: 110), "repeated PINs allowed")
        require(!policy.admit("a", pending: 0, total: 0, now: 110), "reconnect bypass")
        require(policy.canAttempt("b", now: 110), "unrelated peer blocked early")
        for index in 0..<25 { policy.failed("peer-\(index)", now: 110) }
        require(!policy.canAttempt("new", now: 110), "global failure bypass")
        require(policy.admit("a", pending: 0, total: 0, now: 171), "cooldown never expires")
        var connections = MobilePairingAdmission()
        for _ in 0..<12 { require(connections.admit("a", pending: 0, total: 0, now: 100), "early connection limit") }
        require(!connections.admit("a", pending: 0, total: 0, now: 100), "connection churn bypass")
        for index in 0..<48 { require(connections.admit("host-\(index)", pending: 0, total: 0, now: 100), "early global limit") }
        require(!connections.admit("next", pending: 0, total: 0, now: 100), "global connection limit")
        require(!MobilePairingAdmission.accepts(version: 3, submittedPIN: "1234", currentPIN: "1234", pairingActive: true, savedPIN: nil), "wrong protocol consumes pairing")
        require(MobilePairingAdmission.accepts(version: 4, submittedPIN: "1234", currentPIN: "1234", pairingActive: true, savedPIN: nil), "current PIN rejected")
        require(MobilePairingAdmission.accepts(version: 4, submittedPIN: "5678", currentPIN: "1234", pairingActive: false, savedPIN: "5678"), "saved PIN reconnect rejected")
        require(!MobilePairingAdmission.accepts(version: 4, submittedPIN: "1234", currentPIN: "1234", pairingActive: false, savedPIN: "5678"), "closed pairing accepted")
        print("PASS: connection and PIN limits, reconnect protection, cooldown and protocol compatibility")

        var sockets = [Int32](repeating: 0, count: 2)
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0, "socketpair")
        let reader = sockets[0], writer = sockets[1]
        defer { close(reader); close(writer) }
        var yes: Int32 = 1
        _ = setsockopt(writer, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout.size(ofValue: yes)))
        let began = ProcessInfo.processInfo.systemUptime
        let group = DispatchGroup()
        group.enter()
        DispatchQueue.global().async {
            defer { group.leave() }
            for _ in 0..<10 {
                usleep(30_000)
                var byte: UInt8 = 1
                _ = send(writer, &byte, 1, 0)
            }
        }
        do {
            _ = try MobileSocketIO.readExact(reader, count: 20, deadline: began + 0.12)
            preconditionFailure("trickle input did not time out")
        } catch let error as POSIXError { require(error.code == .ETIMEDOUT, "wrong timeout error") }
        require(ProcessInfo.processInfo.systemUptime - began < 0.25, "progress extended the deadline")
        group.wait()
        _ = try MobileSocketIO.readExact(reader, count: 3)
        print("PASS: absolute timeout on real socket I/O; authenticated reads remain untimed")
    }
}
