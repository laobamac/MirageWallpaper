//
//  Mirage Wallpaper
//
//  Native Wallpaper Engine Android discovery and pairing service.
//

import CommonCrypto
import Darwin
import Foundation
import Security

protocol MobilePairingServiceDelegate: AnyObject {
    func mobilePairingServiceDidStart(_ service: MobilePairingService, pin: String)
    func mobilePairingService(_ service: MobilePairingService, didPair device: MobileDevice)
    func mobilePairingService(_ service: MobilePairingService, didDisconnect identifier: String)
    func mobilePairingService(_ service: MobilePairingService, didFail message: String)
    func mobilePairingServiceDidStop(_ service: MobilePairingService)
}

final class MobilePairingService: @unchecked Sendable {
    private struct TransferSession {
        let socket: Int32
        let aesKey: Data
        let iv: Data
    }

    private struct PendingTransfer {
        let fileURL: URL
        let title: String
        let progress: ((UInt64, UInt64) -> Void)?
        let completion: (Result<Void, Error>) -> Void
        let deadline: DispatchTime
    }

    private struct IdentityMetadata: Codable {
        let guid: String
        var pin: String?
        var pairedPins: [String: String]?
    }

    private struct Identity {
        let guid: String
        var pin: String
        let privateKey: SecKey
    }

    private enum Constants {
        static let protocolVersion = 4
        static let tcpPort: UInt16 = 7889
        static let discoveryPort: UInt16 = 7884
        static let multicastAddress = "239.100.0.1"
        static let maximumFrameSize = 1_048_592
        static let maximumCommandSize = 4096
    }

    weak var delegate: MobilePairingServiceDelegate?

    private let queue = DispatchQueue(label: "cn.laobamac.Mirage.mobile-pairing")
    private let clientQueue = DispatchQueue(
        label: "cn.laobamac.Mirage.mobile-pairing.clients",
        qos: .userInitiated,
        attributes: .concurrent
    )
    private var listener: Int32 = -1
    private var discoverySocket: Int32 = -1
    private var listenerSource: DispatchSourceRead?
    private var discoveryTimer: DispatchSourceTimer?
    private var clientSockets = Set<Int32>()
    private var activeDeviceSockets: [String: Int32] = [:]
    private var transferSessions: [String: TransferSession] = [:]
    private var transferringDevices = Set<String>()
    private var pendingTransfers: [String: PendingTransfer] = [:]
    private var pairedPins: [String: String] = [:]
    private var knownPairedDeviceIdentifiers = Set<String>()
    private var identity: Identity?
    private var running = false
    private var pairingSessionActive = false

    private let identityDirectory: URL
    private let metadataURL: URL
    private let privateKeyURL: URL

    init() {
        let support = FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        ).first ?? FileManager.default.homeDirectoryForCurrentUser
        identityDirectory = support
            .appending(path: "Mirage", directoryHint: .isDirectory)
            .appending(path: "MobileTransfer", directoryHint: .isDirectory)
            .appending(path: "Identity", directoryHint: .isDirectory)
        metadataURL = identityDirectory.appending(path: "identity.json")
        privateKeyURL = identityDirectory.appending(path: "private-key.der")
    }

    func startForPairing() {
        queue.async { [weak self] in
            self?.startOnQueue(rotatePINSession: true, announcePIN: true)
        }
    }

    /// Keep discovery and the TCP listener alive independently of the sheet.
    /// Android uses the saved per-device PIN to reconnect after a disconnect.
    func ensureRunning() {
        queue.async { [weak self] in
            self?.startOnQueue(rotatePINSession: false, announcePIN: false)
        }
    }

    func stop() {
        queue.async { [weak self] in
            self?.stopOnQueue(notify: true)
        }
    }

    func endPairingSession() {
        queue.async { [weak self] in
            self?.pairingSessionActive = false
        }
    }

    func sendMPKG(
        at fileURL: URL,
        title: String,
        to identifier: String,
        progress: ((UInt64, UInt64) -> Void)? = nil,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        let deadline = DispatchTime.now() + .seconds(120)
        queue.async { [weak self] in
            guard let self else { return }
            guard !self.transferringDevices.contains(identifier),
                  self.pendingTransfers[identifier] == nil else {
                DispatchQueue.main.async {
                    completion(.failure(MobilePairingError.transferAlreadyRunning))
                }
                return
            }
            self.pendingTransfers[identifier] = PendingTransfer(
                fileURL: fileURL,
                title: title,
                progress: progress,
                completion: completion,
                deadline: deadline
            )
            self.startPendingTransferIfPossible(identifier)
            self.queue.asyncAfter(deadline: deadline) { [weak self] in
                self?.timeoutPendingTransfer(identifier, deadline: deadline)
            }
        }
    }

    private func startPendingTransferIfPossible(_ identifier: String) {
        guard let pending = pendingTransfers[identifier],
              let session = transferSessions[identifier],
              activeDeviceSockets[identifier] == session.socket else { return }
        pendingTransfers.removeValue(forKey: identifier)
        guard transferringDevices.insert(identifier).inserted else {
            DispatchQueue.main.async {
                pending.completion(.failure(MobilePairingError.transferAlreadyRunning))
            }
            return
        }
        clientQueue.async { [weak self] in
            guard let self else { return }
            let result = Result {
                try self.sendMPKGOnClientQueue(
                    pending.fileURL,
                    title: pending.title,
                    session: session,
                    progress: pending.progress
                )
            }
            self.queue.async {
                self.transferringDevices.remove(identifier)
                DispatchQueue.main.async { pending.completion(result) }
            }
        }
    }

    private func timeoutPendingTransfer(_ identifier: String, deadline: DispatchTime) {
        guard let pending = pendingTransfers[identifier],
              pending.deadline.uptimeNanoseconds == deadline.uptimeNanoseconds else { return }
        pendingTransfers.removeValue(forKey: identifier)
        DispatchQueue.main.async {
            pending.completion(.failure(MobilePairingError.deviceNotConnected))
        }
    }

    /// Supplies the device UUIDs restored by the UI. Older Mirage builds only
    /// persisted one shared PIN, so these identifiers let the service migrate
    /// that PIN into the per-device store before Android attempts to reconnect.
    func restorePairings(identifiers: [String]) {
        queue.async { [weak self] in
            guard let self else { return }
            self.knownPairedDeviceIdentifiers = Set(identifiers)
        }
    }

    func removePairing(identifier: String) {
        queue.async { [weak self] in
            guard let self else { return }
            let pending = self.pendingTransfers.removeValue(forKey: identifier)
            self.knownPairedDeviceIdentifiers.remove(identifier)
            self.pairedPins.removeValue(forKey: identifier)
            self.transferSessions.removeValue(forKey: identifier)
            self.transferringDevices.remove(identifier)
            if let socket = self.activeDeviceSockets.removeValue(forKey: identifier) {
                Darwin.shutdown(socket, SHUT_RDWR)
            }
            self.persistMetadataOnQueue()
            if let pending {
                DispatchQueue.main.async {
                    pending.completion(.failure(MobilePairingError.deviceNotConnected))
                }
            }
        }
    }

    private func startOnQueue(rotatePINSession: Bool, announcePIN: Bool) {
        guard !running else {
            if rotatePINSession, var identity {
                do {
                    identity.pin = try rotatePIN(for: identity.guid, excluding: identity.pin)
                    self.identity = identity
                    pairingSessionActive = true
                    if announcePIN {
                        notifyOnMain { $0.mobilePairingServiceDidStart(self, pin: identity.pin) }
                    }
                } catch {
                    notifyFailure(error.localizedDescription)
                }
            }
            return
        }

        do {
            var identity = try loadOrCreateIdentity()
            // A PIN is a pairing-session secret. Keep the computer identity
            // (GUID and RSA key) stable so Android can recognize this Mac, but
            // issue a fresh PIN whenever the user starts a new pairing flow.
            if rotatePINSession {
                identity.pin = try rotatePIN(for: identity.guid, excluding: identity.pin)
                pairingSessionActive = true
            }
            let listener = try makeListener()
            let discoverySocket = try makeDiscoverySocket()

            self.identity = identity
            self.listener = listener
            self.discoverySocket = discoverySocket
            running = true

            let source = DispatchSource.makeReadSource(fileDescriptor: listener, queue: queue)
            source.setEventHandler { [weak self] in self?.acceptConnections() }
            source.setCancelHandler { Darwin.close(listener) }
            listenerSource = source
            source.resume()

            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now(), repeating: 1.25, leeway: .milliseconds(100))
            timer.setEventHandler { [weak self] in self?.broadcastDiscovery() }
            discoveryTimer = timer
            timer.resume()

            if announcePIN {
                notifyOnMain { $0.mobilePairingServiceDidStart(self, pin: identity.pin) }
            }
        } catch {
            stopOnQueue(notify: false)
            notifyFailure(error.localizedDescription)
        }
    }

    private func stopOnQueue(notify: Bool) {
        guard running || listener >= 0 || discoverySocket >= 0 else {
            if notify { notifyOnMain { $0.mobilePairingServiceDidStop(self) } }
            return
        }
        running = false
        pairingSessionActive = false

        let pending = pendingTransfers.values
        pendingTransfers.removeAll()
        for transfer in pending {
            DispatchQueue.main.async {
                transfer.completion(.failure(MobilePairingError.deviceNotConnected))
            }
        }

        discoveryTimer?.cancel()
        discoveryTimer = nil
        listenerSource?.cancel()
        listenerSource = nil

        if discoverySocket >= 0 {
            Darwin.close(discoverySocket)
            discoverySocket = -1
        }
        listener = -1

        let sockets = clientSockets
        clientSockets.removeAll()
        activeDeviceSockets.removeAll()
        transferSessions.removeAll()
        transferringDevices.removeAll()
        for socket in sockets {
            Darwin.shutdown(socket, SHUT_RDWR)
        }
        if notify { notifyOnMain { $0.mobilePairingServiceDidStop(self) } }
    }

    private func makeListener() throws -> Int32 {
        let descriptor = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        guard descriptor >= 0 else { throw POSIXError(.init(rawValue: errno) ?? .EIO) }
        do {
            var yes: Int32 = 1
            guard setsockopt(
                descriptor,
                SOL_SOCKET,
                SO_REUSEADDR,
                &yes,
                socklen_t(MemoryLayout.size(ofValue: yes))
            ) == 0 else {
                throw POSIXError(.init(rawValue: errno) ?? .EIO)
            }
            _ = setsockopt(
                descriptor,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &yes,
                socklen_t(MemoryLayout.size(ofValue: yes))
            )

            var address = sockaddr_in()
            address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            address.sin_family = sa_family_t(AF_INET)
            address.sin_port = Constants.tcpPort.bigEndian
            address.sin_addr = in_addr(s_addr: INADDR_ANY)
            let bindResult = withUnsafePointer(to: &address) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.bind(descriptor, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            guard bindResult == 0 else { throw POSIXError(.init(rawValue: errno) ?? .EIO) }
            guard Darwin.listen(descriptor, SOMAXCONN) == 0 else {
                throw POSIXError(.init(rawValue: errno) ?? .EIO)
            }
            let flags = fcntl(descriptor, F_GETFL, 0)
            guard flags >= 0, fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0 else {
                throw POSIXError(.init(rawValue: errno) ?? .EIO)
            }
            return descriptor
        } catch {
            Darwin.close(descriptor)
            throw error
        }
    }

    private func makeDiscoverySocket() throws -> Int32 {
        let descriptor = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard descriptor >= 0 else { throw POSIXError(.init(rawValue: errno) ?? .EIO) }
        var yes: Int32 = 1
        guard setsockopt(
            descriptor,
            SOL_SOCKET,
            SO_BROADCAST,
            &yes,
            socklen_t(MemoryLayout.size(ofValue: yes))
        ) == 0 else {
            let code = errno
            Darwin.close(descriptor)
            throw POSIXError(.init(rawValue: code) ?? .EIO)
        }
        var ttl: UInt8 = 1
        _ = setsockopt(
            descriptor,
            IPPROTO_IP,
            IP_MULTICAST_TTL,
            &ttl,
            socklen_t(MemoryLayout.size(ofValue: ttl))
        )
        return descriptor
    }

    private func acceptConnections() {
        guard listener >= 0, let identity else { return }
        while true {
            var address = sockaddr_in()
            var length = socklen_t(MemoryLayout<sockaddr_in>.size)
            let client = withUnsafeMutablePointer(to: &address) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.accept(listener, $0, &length)
                }
            }
            if client < 0 {
                if errno == EAGAIN || errno == EWOULDBLOCK { return }
                notifyFailure(POSIXError(.init(rawValue: errno) ?? .EIO).localizedDescription)
                return
            }

            var yes: Int32 = 1
            _ = setsockopt(
                client,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &yes,
                socklen_t(MemoryLayout.size(ofValue: yes))
            )
            // On Darwin an accepted socket can inherit O_NONBLOCK from the
            // listening descriptor. The Android client needs a short moment
            // after receiving our RSA public key to encrypt its UUID, IV and
            // AES key. A non-blocking recv here returns EAGAIN immediately and
            // makes the phone report a generic connection failure.
            let clientFlags = fcntl(client, F_GETFL, 0)
            if clientFlags >= 0 {
                _ = fcntl(client, F_SETFL, clientFlags & ~O_NONBLOCK)
            }
            clientSockets.insert(client)
            clientQueue.async { [weak self] in
                self?.handleClient(client, identity: identity)
            }
        }
    }

    private func handleClient(_ socket: Int32, identity: Identity) {
        var pairedIdentifier: String?
        defer {
            Darwin.shutdown(socket, SHUT_RDWR)
            Darwin.close(socket)
            let wasActive = queue.sync { [weak self] () -> Bool in
                guard let self else { return false }
                self.clientSockets.remove(socket)
                guard let pairedIdentifier,
                      self.activeDeviceSockets[pairedIdentifier] == socket else { return false }
                self.activeDeviceSockets.removeValue(forKey: pairedIdentifier)
                self.transferSessions.removeValue(forKey: pairedIdentifier)
                self.transferringDevices.remove(pairedIdentifier)
                return true
            }
            if wasActive, let pairedIdentifier {
                notifyOnMain {
                    $0.mobilePairingService(self, didDisconnect: pairedIdentifier)
                }
            }
        }

        do {
            try writeAll(publicKeyWireBytes(identity.privateKey), to: socket)
            let identifierData = try rsaDecrypt(try readFrame(socket, maximum: Constants.maximumCommandSize), key: identity.privateKey)
            let iv = try rsaDecrypt(try readFrame(socket, maximum: Constants.maximumCommandSize), key: identity.privateKey)
            let aesKey = try rsaDecrypt(try readFrame(socket, maximum: Constants.maximumCommandSize), key: identity.privateKey)

            guard let identifier = String(data: identifierData, encoding: .utf8), !identifier.isEmpty else {
                throw MobilePairingError.invalidClientData
            }
            guard iv.count == kCCBlockSizeAES128, aesKey.count == kCCKeySizeAES256 else {
                throw MobilePairingError.invalidSessionMaterial
            }

            while true {
                let request = try readEncryptedJSON(socket, key: aesKey, iv: iv)
                let version = request["version"] as? Int
                let submittedPIN = String(describing: request["pin"] ?? "")
                let acceptedPIN = queue.sync { [weak self] in
                    guard let self else { return false }
                    let isCurrentPairingPIN = self.pairingSessionActive
                        && submittedPIN == self.identity?.pin
                    let isSavedDevicePIN = self.pairedPins[identifier] == submittedPIN
                    if isCurrentPairingPIN {
                        // A pairing screen represents one new-device pairing
                        // session. Existing devices continue using their own
                        // saved PINs after this session has been consumed.
                        self.pairingSessionActive = false
                    }
                    return isCurrentPairingPIN || isSavedDevicePIN
                }
                let accepted = version == Constants.protocolVersion && acceptedPIN
                let status: String
                if version != Constants.protocolVersion {
                    status = "AuthFailedVersion"
                } else if !accepted {
                    status = "AuthFailed"
                } else {
                    status = "AuthOK"
                }
                try sendEncryptedJSON(["status": status], to: socket, key: aesKey, iv: iv)
                guard accepted else { continue }

                // Android reports the phone model as `name` and the user's
                // chosen phone name as `deviceUserName`.
                let name = nonEmptyString(request["deviceUserName"])
                    ?? nonEmptyString(request["name"])
                    ?? "Android"
                let model = nonEmptyString(request["name"])
                    ?? nonEmptyString(request["model"])
                    ?? "Android"
                pairedIdentifier = identifier
                let previousSocket = queue.sync { [weak self] () -> Int32? in
                    guard let self else { return nil }
                    let previous = self.activeDeviceSockets.updateValue(socket, forKey: identifier)
                    self.transferSessions[identifier] = TransferSession(
                        socket: socket,
                        aesKey: aesKey,
                        iv: iv
                    )
                    self.pairedPins[identifier] = submittedPIN
                    self.persistMetadataOnQueue()
                    return previous == socket ? nil : previous
                }
                if let previousSocket {
                    Darwin.shutdown(previousSocket, SHUT_RDWR)
                }
                let device = MobileDevice(
                    id: identifier,
                    name: name,
                    model: model,
                    isConnected: true
                )
                notifyOnMain { $0.mobilePairingService(self, didPair: device) }
                queue.async { [weak self] in
                    self?.startPendingTransferIfPossible(identifier)
                }
                break
            }

            var buffer = [UInt8](repeating: 0, count: 4096)
            while true {
                let count = Darwin.recv(socket, &buffer, buffer.count, 0)
                if count <= 0 { return }
            }
        } catch MobilePairingError.connectionClosed {
            // Closing the sheet, cancelling pairing, and a phone leaving the
            // network all end the TCP stream normally. They should update the
            // connection state without presenting an error alert.
        } catch is POSIXError {
            // Socket-level disconnects are expected on a local mobile
            // connection. Protocol and cryptography failures still surface to
            // the user through the general catch below.
        } catch {
            notifyFailure(error.localizedDescription)
        }
    }

    private func broadcastDiscovery() {
        guard discoverySocket >= 0, let identity else { return }
        let computerName = Host.current().localizedName?
            .replacingOccurrences(of: ":", with: "-")
            .prefix(48) ?? "Mirage"
        let payload = Data(
            "WallpaperEngine/Ping:\(String(format: "%05d", Constants.tcpPort)):\(computerName):\(Constants.protocolVersion):\(identity.guid)"
                .utf8
        )
        var destinations = Set<String>([Constants.multicastAddress, "255.255.255.255"])
        destinations.formUnion(interfaceBroadcastAddresses())
        for destination in destinations {
            sendDatagram(payload, address: destination)
        }
    }

    private func sendDatagram(_ payload: Data, address: String) {
        var destination = sockaddr_in()
        destination.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        destination.sin_family = sa_family_t(AF_INET)
        destination.sin_port = Constants.discoveryPort.bigEndian
        guard address.withCString({ inet_pton(AF_INET, $0, &destination.sin_addr) }) == 1 else { return }
        payload.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress else { return }
            _ = withUnsafePointer(to: &destination) { pointer in
                pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.sendto(
                        discoverySocket,
                        base,
                        payload.count,
                        0,
                        $0,
                        socklen_t(MemoryLayout<sockaddr_in>.size)
                    )
                }
            }
        }
    }

    private func interfaceBroadcastAddresses() -> Set<String> {
        var result = Set<String>()
        var interfaces: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&interfaces) == 0, let first = interfaces else { return result }
        defer { freeifaddrs(interfaces) }

        var current: UnsafeMutablePointer<ifaddrs>? = first
        while let interface = current?.pointee {
            defer { current = interface.ifa_next }
            let flags = Int32(interface.ifa_flags)
            guard flags & IFF_UP != 0,
                  flags & IFF_LOOPBACK == 0,
                  flags & IFF_BROADCAST != 0,
                  let broadcast = interface.ifa_dstaddr,
                  broadcast.pointee.sa_family == UInt8(AF_INET) else { continue }
            var address = broadcast.pointee
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(
                &address,
                socklen_t(address.sa_len),
                &host,
                socklen_t(host.count),
                nil,
                0,
                NI_NUMERICHOST
            ) == 0 {
                result.insert(String(cString: host))
            }
        }
        return result
    }

    private func loadOrCreateIdentity() throws -> Identity {
        try FileManager.default.createDirectory(
            at: identityDirectory,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700]
        )

        let metadata: IdentityMetadata
        if let data = try? Data(contentsOf: metadataURL),
           let saved = try? JSONDecoder().decode(IdentityMetadata.self, from: data) {
            metadata = saved
        } else {
            metadata = IdentityMetadata(
                guid: UUID().uuidString.uppercased(),
                pin: nil,
                pairedPins: [:]
            )
            let data = try JSONEncoder().encode(metadata)
            try data.write(to: metadataURL, options: .atomic)
            try? FileManager.default.setAttributes(
                [.posixPermissions: 0o600],
                ofItemAtPath: metadataURL.path
            )
        }

        let privateKey: SecKey
        if let data = try? Data(contentsOf: privateKeyURL),
           let key = SecKeyCreateWithData(
               data as CFData,
               [
                   kSecAttrKeyType: kSecAttrKeyTypeRSA,
                   kSecAttrKeyClass: kSecAttrKeyClassPrivate,
                   kSecAttrKeySizeInBits: 1024,
               ] as CFDictionary,
               nil
           ) {
            privateKey = key
        } else {
            var error: Unmanaged<CFError>?
            guard let key = SecKeyCreateRandomKey(
                [
                    kSecAttrKeyType: kSecAttrKeyTypeRSA,
                    kSecAttrKeySizeInBits: 1024,
                ] as CFDictionary,
                &error
            ) else {
                if let error { throw error.takeRetainedValue() }
                throw MobilePairingError.keyGenerationFailed
            }
            guard let external = SecKeyCopyExternalRepresentation(key, &error) as Data? else {
                if let error { throw error.takeRetainedValue() }
                throw MobilePairingError.keyGenerationFailed
            }
            try external.write(to: privateKeyURL, options: .atomic)
            try? FileManager.default.setAttributes(
                [.posixPermissions: 0o600],
                ofItemAtPath: privateKeyURL.path
            )
            privateKey = key
        }
        pairedPins = metadata.pairedPins ?? [:]
        if metadata.pairedPins == nil,
           let legacyPIN = metadata.pin,
           !knownPairedDeviceIdentifiers.isEmpty {
            for identifier in knownPairedDeviceIdentifiers {
                pairedPins[identifier] = legacyPIN
            }
            try writeMetadata(
                IdentityMetadata(
                    guid: metadata.guid,
                    pin: legacyPIN,
                    pairedPins: pairedPins
                )
            )
        }
        return Identity(guid: metadata.guid, pin: metadata.pin ?? Self.makePIN(), privateKey: privateKey)
    }

    private func rotatePIN(for guid: String, excluding previousPIN: String?) throws -> String {
        var pin = Self.makePIN()
        while pin == previousPIN {
            pin = Self.makePIN()
        }
        let metadata = IdentityMetadata(guid: guid, pin: pin, pairedPins: pairedPins)
        try writeMetadata(metadata)
        return pin
    }

    private func persistMetadataOnQueue() {
        guard let identity else { return }
        let metadata = IdentityMetadata(
            guid: identity.guid,
            pin: identity.pin,
            pairedPins: pairedPins
        )
        try? writeMetadata(metadata)
    }

    private func writeMetadata(_ metadata: IdentityMetadata) throws {
        let data = try JSONEncoder().encode(metadata)
        try data.write(to: metadataURL, options: .atomic)
        try? FileManager.default.setAttributes(
            [.posixPermissions: 0o600],
            ofItemAtPath: metadataURL.path
        )
    }

    private static func makePIN() -> String {
        String(format: "%04d", Int.random(in: 0...9999))
    }

    private func publicKeyWireBytes(_ privateKey: SecKey) throws -> Data {
        guard let publicKey = SecKeyCopyPublicKey(privateKey) else {
            throw MobilePairingError.keyGenerationFailed
        }
        var error: Unmanaged<CFError>?
        guard let external = SecKeyCopyExternalRepresentation(publicKey, &error) as Data? else {
            if let error { throw error.takeRetainedValue() }
            throw MobilePairingError.invalidPublicKey
        }
        let integers = try DERReader(data: external).rsaPublicKeyIntegers()
        var result = Data()
        result.appendLittleEndian(UInt32(integers.exponent.count))
        result.append(integers.exponent)
        result.appendLittleEndian(UInt32(integers.modulus.count))
        result.append(integers.modulus)
        return result
    }

    private func rsaDecrypt(_ data: Data, key: SecKey) throws -> Data {
        var error: Unmanaged<CFError>?
        guard let decrypted = SecKeyCreateDecryptedData(
            key,
            .rsaEncryptionOAEPSHA1,
            data as CFData,
            &error
        ) as Data? else {
            if let error { throw error.takeRetainedValue() }
            throw MobilePairingError.rsaDecryptionFailed
        }
        return decrypted
    }

    private func readEncryptedJSON(_ socket: Int32, key: Data, iv: Data) throws -> [String: Any] {
        let encrypted = try readFrame(socket, maximum: Constants.maximumCommandSize)
        let plaintext = try crypt(encrypted, key: key, iv: iv, operation: CCOperation(kCCDecrypt))
        guard let value = try JSONSerialization.jsonObject(with: plaintext) as? [String: Any] else {
            throw MobilePairingError.invalidClientData
        }
        return value
    }

    private func sendEncryptedJSON(
        _ value: [String: Any],
        to socket: Int32,
        key: Data,
        iv: Data
    ) throws {
        let plaintext = try JSONSerialization.data(withJSONObject: value)
        let encrypted = try crypt(plaintext, key: key, iv: iv, operation: CCOperation(kCCEncrypt))
        try writeFrame(encrypted, to: socket)
    }

    private func sendEncryptedData(
        _ payload: Data,
        to socket: Int32,
        key: Data,
        iv: Data
    ) throws {
        let encrypted = try crypt(payload, key: key, iv: iv, operation: CCOperation(kCCEncrypt))
        guard encrypted.count <= Constants.maximumFrameSize else {
            throw MobilePairingError.invalidFrame
        }
        try writeFrame(encrypted, to: socket)
    }

    private func sendMPKGOnClientQueue(
        _ fileURL: URL,
        title: String,
        session: TransferSession,
        progress: ((UInt64, UInt64) -> Void)?
    ) throws {
        let values = try fileURL.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
        guard values.isRegularFile == true, let fileSize = values.fileSize else {
            throw MobilePairingError.transferFileUnavailable
        }
        progress?(0, UInt64(fileSize))
        let remoteName = fileURL.lastPathComponent
        let common: [String: Any] = [
            "title": title,
            "file": remoteName,
            "size": fileSize,
        ]
        try sendEncryptedJSON(
            common.merging(["command": "beginUpload"]) { current, _ in current },
            to: session.socket,
            key: session.aesKey,
            iv: session.iv
        )
        try sendEncryptedJSON(
            common.merging(["command": "transmissionStart"]) { current, _ in current },
            to: session.socket,
            key: session.aesKey,
            iv: session.iv
        )

        let input = try FileHandle(forReadingFrom: fileURL)
        defer { try? input.close() }
        var sentAny = false
        var sent: UInt64 = 0
        var lastReported: UInt64 = 0
        let progressInterval: UInt64 = 1024 * 1024
        while true {
            let chunk = try input.read(upToCount: 128 * 1024) ?? Data()
            if chunk.isEmpty { break }
            sentAny = true
            try sendEncryptedJSON(
                ["command": "transmissionContinue", "size": chunk.count],
                to: session.socket,
                key: session.aesKey,
                iv: session.iv
            )
            try sendEncryptedData(
                chunk,
                to: session.socket,
                key: session.aesKey,
                iv: session.iv
            )
            sent += UInt64(chunk.count)
            if sent == UInt64(fileSize) || sent - lastReported >= progressInterval {
                lastReported = sent
                progress?(sent, UInt64(fileSize))
            }
        }
        if !sentAny {
            try sendEncryptedJSON(
                ["command": "transmissionContinue", "size": 0],
                to: session.socket,
                key: session.aesKey,
                iv: session.iv
            )
        }
    }

    private func crypt(_ input: Data, key: Data, iv: Data, operation: CCOperation) throws -> Data {
        var output = Data(count: input.count + kCCBlockSizeAES128)
        let outputCapacity = output.count
        var outputLength = 0
        let status = output.withUnsafeMutableBytes { outputBytes in
            input.withUnsafeBytes { inputBytes in
                key.withUnsafeBytes { keyBytes in
                    iv.withUnsafeBytes { ivBytes in
                        CCCrypt(
                            operation,
                            CCAlgorithm(kCCAlgorithmAES),
                            CCOptions(kCCOptionPKCS7Padding),
                            keyBytes.baseAddress,
                            key.count,
                            ivBytes.baseAddress,
                            inputBytes.baseAddress,
                            input.count,
                            outputBytes.baseAddress,
                            outputCapacity,
                            &outputLength
                        )
                    }
                }
            }
        }
        guard status == kCCSuccess else { throw MobilePairingError.encryptionFailed(status) }
        output.count = outputLength
        return output
    }

    private func readFrame(_ socket: Int32, maximum: Int) throws -> Data {
        let header = try readExact(socket, count: 4)
        let length = Int(header.littleEndianUInt32)
        guard length > 0, length <= maximum else { throw MobilePairingError.invalidFrame }
        return try readExact(socket, count: length)
    }

    private func writeFrame(_ payload: Data, to socket: Int32) throws {
        var frame = Data()
        frame.appendLittleEndian(UInt32(payload.count))
        frame.append(payload)
        try writeAll(frame, to: socket)
    }

    private func readExact(_ socket: Int32, count: Int) throws -> Data {
        var result = Data(count: count)
        var received = 0
        try result.withUnsafeMutableBytes { bytes in
            guard let base = bytes.baseAddress else { return }
            while received < count {
                let amount = Darwin.recv(socket, base.advanced(by: received), count - received, 0)
                if amount == 0 { throw MobilePairingError.connectionClosed }
                if amount < 0 {
                    if errno == EINTR { continue }
                    throw POSIXError(.init(rawValue: errno) ?? .EIO)
                }
                received += amount
            }
        }
        return result
    }

    private func writeAll(_ data: Data, to socket: Int32) throws {
        var sent = 0
        try data.withUnsafeBytes { bytes in
            guard let base = bytes.baseAddress else { return }
            while sent < data.count {
                let amount = Darwin.send(socket, base.advanced(by: sent), data.count - sent, 0)
                if amount == 0 { throw MobilePairingError.connectionClosed }
                if amount < 0 {
                    if errno == EINTR { continue }
                    throw POSIXError(.init(rawValue: errno) ?? .EIO)
                }
                sent += amount
            }
        }
    }

    private func nonEmptyString(_ value: Any?) -> String? {
        guard let string = value as? String, !string.isEmpty else { return nil }
        return string
    }

    private func notifyFailure(_ message: String) {
        notifyOnMain { $0.mobilePairingService(self, didFail: message) }
    }

    private func notifyOnMain(_ action: @escaping (MobilePairingServiceDelegate) -> Void) {
        DispatchQueue.main.async { [weak self] in
            guard let delegate = self?.delegate else { return }
            action(delegate)
        }
    }
}

enum MobilePairingError: LocalizedError {
    case invalidFrame
    case connectionClosed
    case invalidClientData
    case invalidSessionMaterial
    case invalidPublicKey
    case keyGenerationFailed
    case rsaDecryptionFailed
    case encryptionFailed(CCCryptorStatus)
    case deviceNotConnected
    case transferAlreadyRunning
    case transferFileUnavailable

    var errorDescription: String? {
        switch self {
        case .invalidFrame: return L("移动设备发送了无效的数据帧")
        case .connectionClosed: return L("移动设备已断开连接")
        case .invalidClientData: return L("移动设备发送了无效的配对数据")
        case .invalidSessionMaterial: return L("移动设备发送了无效的加密会话")
        case .invalidPublicKey: return L("无法读取移动设备配对公钥")
        case .keyGenerationFailed: return L("无法生成移动设备配对身份")
        case .rsaDecryptionFailed: return L("无法解密移动设备配对请求")
        case .encryptionFailed(let status): return L("移动设备加密失败：%d", status)
        case .deviceNotConnected: return L("移动设备当前未连接。")
        case .transferAlreadyRunning: return L("该移动设备已有传输任务正在进行。")
        case .transferFileUnavailable: return L("无法读取要发送的 MPKG 文件。")
        }
    }
}

private struct DERReader {
    let data: Data

    func rsaPublicKeyIntegers() throws -> (modulus: Data, exponent: Data) {
        var offset = 0
        let sequence = try readElement(expectedTag: 0x30, offset: &offset, source: data)
        var sequenceOffset = 0
        var modulus = try readElement(expectedTag: 0x02, offset: &sequenceOffset, source: sequence)
        var exponent = try readElement(expectedTag: 0x02, offset: &sequenceOffset, source: sequence)
        while modulus.first == 0 { modulus.removeFirst() }
        while exponent.first == 0 { exponent.removeFirst() }
        guard !modulus.isEmpty, !exponent.isEmpty else { throw MobilePairingError.invalidPublicKey }
        return (modulus, exponent)
    }

    private func readElement(expectedTag: UInt8, offset: inout Int, source: Data) throws -> Data {
        guard offset < source.count, source[offset] == expectedTag else {
            throw MobilePairingError.invalidPublicKey
        }
        offset += 1
        let length = try readLength(offset: &offset, source: source)
        guard length >= 0, offset + length <= source.count else {
            throw MobilePairingError.invalidPublicKey
        }
        defer { offset += length }
        return source.subdata(in: offset..<(offset + length))
    }

    private func readLength(offset: inout Int, source: Data) throws -> Int {
        guard offset < source.count else { throw MobilePairingError.invalidPublicKey }
        let first = source[offset]
        offset += 1
        if first & 0x80 == 0 { return Int(first) }
        let count = Int(first & 0x7f)
        guard count > 0, count <= 4, offset + count <= source.count else {
            throw MobilePairingError.invalidPublicKey
        }
        var length = 0
        for _ in 0..<count {
            length = (length << 8) | Int(source[offset])
            offset += 1
        }
        return length
    }
}

private extension Data {
    mutating func appendLittleEndian(_ value: UInt32) {
        var value = value.littleEndian
        Swift.withUnsafeBytes(of: &value) { append(contentsOf: $0) }
    }

    var littleEndianUInt32: UInt32 {
        withUnsafeBytes { bytes in
            bytes.loadUnaligned(as: UInt32.self).littleEndian
        }
    }
}
