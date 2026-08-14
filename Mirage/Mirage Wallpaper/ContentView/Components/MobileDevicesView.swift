//
//  Mirage Wallpaper
//
//  Mobile device pairing UI inspired by the Wallpaper Engine desktop client.
//

import AppKit
import SwiftUI

struct MobileDevice: Identifiable, Codable, Equatable {
    let id: String
    let name: String
    let model: String
    var isConnected: Bool

    init(id: String = UUID().uuidString, name: String, model: String, isConnected: Bool = true) {
        self.id = id
        self.name = name
        self.model = model
        self.isConnected = isConnected
    }

    // Used by previews to make the connected-device state easy to inspect.
    static let preview = MobileDevice(
        name: "HUAWEI Nova 11 SE",
        model: "HONOR AAK-AN00"
    )
}

enum MobileDevicesScreen: Equatable {
    case welcome
    case pairing(pin: String)
    case devices
}

final class MobileDevicesViewModel: ObservableObject, MobilePairingServiceDelegate {
    @Published var screen: MobileDevicesScreen
    @Published private(set) var devices: [MobileDevice]
    @Published private(set) var isStartingPairing = false
    @Published var errorMessage: String?

    private let pairingService: MobilePairingService
    private let savedDevicesKey = "MobileTransferPairedDevices"
    private let savedDevicesSchemaKey = "MobileTransferPairedDevicesSchema"
    private static let currentSavedDevicesSchema = 2

    init(
        screen: MobileDevicesScreen = .welcome,
        devices: [MobileDevice]? = nil,
        pairingService: MobilePairingService = MobilePairingService()
    ) {
        self.pairingService = pairingService
        let usesPersistedDevices = devices == nil
        let restoredDevices = devices ?? Self.loadSavedDevices(key: savedDevicesKey)
        let normalizedDevices: [MobileDevice]
        if usesPersistedDevices,
           UserDefaults.standard.integer(forKey: savedDevicesSchemaKey) < Self.currentSavedDevicesSchema {
            // Schema 1 stored the Android protocol fields backwards: `name`
            // contained the model and `model` contained the user-visible phone
            // name. Swap once so existing pairings do not need to be recreated.
            normalizedDevices = restoredDevices.map {
                MobileDevice(
                    id: $0.id,
                    name: $0.model,
                    model: $0.name,
                    isConnected: $0.isConnected
                )
            }
            if let data = try? JSONEncoder().encode(normalizedDevices) {
                UserDefaults.standard.set(data, forKey: savedDevicesKey)
            }
            UserDefaults.standard.set(Self.currentSavedDevicesSchema, forKey: savedDevicesSchemaKey)
        } else {
            normalizedDevices = restoredDevices
        }
        self.devices = normalizedDevices.map {
            MobileDevice(id: $0.id, name: $0.name, model: $0.model, isConnected: false)
        }
        self.screen = screen == .welcome && !normalizedDevices.isEmpty ? .devices : screen
        pairingService.delegate = self
        pairingService.restorePairings(identifiers: normalizedDevices.map(\.id))
    }

    func beginPairing() {
        guard !isStartingPairing else { return }
        errorMessage = nil
        isStartingPairing = true
        pairingService.startForPairing()
    }

    func cancelPairing() {
        isStartingPairing = false
        screen = devices.isEmpty ? .welcome : .devices
        pairingService.endPairingSession()
        if devices.isEmpty {
            pairingService.stop()
        } else {
            pairingService.ensureRunning()
        }
    }

    func startBackgroundReconnect() {
        guard !devices.isEmpty else { return }
        pairingService.ensureRunning()
    }

    func closePairingUI() {
        isStartingPairing = false
        pairingService.endPairingSession()
        if devices.isEmpty {
            pairingService.stop()
        } else {
            pairingService.ensureRunning()
        }
    }

    func send(
        wallpaper: WEWallpaper,
        to device: MobileDevice,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        let progressModel = MobileTransferProgressModel.shared
        let progressID = progressModel.start(
            wallpaperTitle: wallpaper.project.title,
            deviceName: device.name,
            initialPhase: wallpaper.kind == .scene ? .converting : .preparing
        )
        guard device.isConnected else {
            progressModel.fail(
                id: progressID,
                message: MobilePairingError.deviceNotConnected.localizedDescription
            )
            completion(.failure(MobilePairingError.deviceNotConnected))
            return
        }
        let directory = FileManager.default.temporaryDirectory
            .appending(path: "Mirage-Mobile-Transfer", directoryHint: .isDirectory)
            .appending(path: UUID().uuidString, directoryHint: .isDirectory)
        let output = directory.appending(path: MobileMPKGExporter.suggestedFilename(for: wallpaper))

        DispatchQueue.global(qos: .userInitiated).async { [pairingService] in
            do {
                try FileManager.default.createDirectory(
                    at: directory,
                    withIntermediateDirectories: true
                )
                switch wallpaper.kind {
                case .video:
                    try MobileMPKGExporter.export(wallpaper, to: output) { completed, total in
                        progressModel.updatePreparation(
                            id: progressID,
                            completedBytes: completed,
                            totalBytes: total
                        )
                    }
                case .scene:
                    try SceneMobileMPKGExporter.export(wallpaper, to: output) { fraction in
                        progressModel.updateConversion(id: progressID, fraction: fraction)
                    }
                case .web, .unsupported:
                    throw MobileMPKGExportError.unsupportedWallpaperType(wallpaper.kind)
                }
                progressModel.waitForDevice(id: progressID)
                pairingService.sendMPKG(
                    at: output,
                    title: wallpaper.project.title,
                    to: device.id,
                    progress: { completed, total in
                        progressModel.updateUpload(
                            id: progressID,
                            completedBytes: completed,
                            totalBytes: total
                        )
                    }
                ) { result in
                    try? FileManager.default.removeItem(at: directory)
                    switch result {
                    case .success:
                        progressModel.complete(id: progressID)
                    case .failure(let error):
                        progressModel.fail(id: progressID, message: error.localizedDescription)
                    }
                    completion(result)
                }
            } catch {
                try? FileManager.default.removeItem(at: directory)
                progressModel.fail(id: progressID, message: error.localizedDescription)
                DispatchQueue.main.async { completion(.failure(error)) }
            }
        }
    }

    func removeDevice(id: MobileDevice.ID) {
        devices.removeAll { $0.id == id }
        pairingService.removePairing(identifier: id)
        saveDevices()
        if devices.isEmpty {
            pairingService.stop()
            screen = .welcome
        }
    }

    func stopPairing() {
        pairingService.stop()
        isStartingPairing = false
        devices = devices.map {
            MobileDevice(id: $0.id, name: $0.name, model: $0.model, isConnected: false)
        }
    }

    func mobilePairingServiceDidStart(_ service: MobilePairingService, pin: String) {
        isStartingPairing = false
        screen = .pairing(pin: pin)
    }

    func mobilePairingService(_ service: MobilePairingService, didPair device: MobileDevice) {
        isStartingPairing = false
        if let index = devices.firstIndex(where: { $0.id == device.id }) {
            devices[index] = device
        } else {
            devices.append(device)
        }
        saveDevices()
        screen = .devices
    }

    func mobilePairingService(
        _ service: MobilePairingService,
        didDisconnect identifier: String
    ) {
        guard let index = devices.firstIndex(where: { $0.id == identifier }) else { return }
        let device = devices[index]
        devices[index] = MobileDevice(
            id: device.id,
            name: device.name,
            model: device.model,
            isConnected: false
        )
    }

    func mobilePairingService(_ service: MobilePairingService, didFail message: String) {
        isStartingPairing = false
        errorMessage = message
    }

    func mobilePairingServiceDidStop(_ service: MobilePairingService) {
        isStartingPairing = false
    }

    private func saveDevices() {
        let persistent = devices.map {
            MobileDevice(id: $0.id, name: $0.name, model: $0.model, isConnected: false)
        }
        guard let data = try? JSONEncoder().encode(persistent) else { return }
        UserDefaults.standard.set(data, forKey: savedDevicesKey)
        UserDefaults.standard.set(Self.currentSavedDevicesSchema, forKey: savedDevicesSchemaKey)
    }

    private static func loadSavedDevices(key: String) -> [MobileDevice] {
        guard let data = UserDefaults.standard.data(forKey: key),
              let devices = try? JSONDecoder().decode([MobileDevice].self, from: data) else {
            return []
        }
        return devices
    }
}

struct MobileDevicesView: View {
    @Environment(\.dismiss) private var dismiss
    @ObservedObject var viewModel: MobileDevicesViewModel

    private let background = Color(nsColor: .windowBackgroundColor)

    init(
        initialScreen: MobileDevicesScreen = .welcome,
        devices: [MobileDevice] = []
    ) {
        viewModel = MobileDevicesViewModel(screen: initialScreen, devices: devices)
    }

    init(viewModel: MobileDevicesViewModel) {
        self.viewModel = viewModel
    }

    var body: some View {
        ZStack {
            background
                .ignoresSafeArea()

            Group {
                switch viewModel.screen {
                case .welcome:
                    welcomePage
                case .pairing(let pin):
                    pairingPage(pin: pin)
                case .devices:
                    devicesPage
                }
            }
            .frame(maxWidth: 1_000, maxHeight: 1_000)
        }
        .overlay(alignment: .topTrailing) {
            closeButton
                .padding(14)
        }
        .frame(width: 560, height: 460)
        .foregroundStyle(.primary)
        .alert(
            "移动设备连接失败",
            isPresented: Binding(
                get: { viewModel.errorMessage != nil },
                set: { if !$0 { viewModel.errorMessage = nil } }
            )
        ) {
            Button("好", role: .cancel) { viewModel.errorMessage = nil }
        } message: {
            Text(viewModel.errorMessage ?? "")
        }
        .onDisappear {
            viewModel.closePairingUI()
        }
    }

    private var welcomePage: some View {
        VStack(spacing: 0) {
            pageTitle("移动设备")

            Text("在 Android 手机或平板电脑上免费安装 Wallpaper Engine，即可随时随地使用动画壁纸。")
                .font(.body)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 520)
                .padding(.top, 12)

            Spacer(minLength: 16)

            downloadCard

            Spacer(minLength: 20)

            Text("右键单击兼容的壁纸并选择“发送到移动设备”，以将壁纸无线传输到您的手机或平板电脑。")
                .font(.body)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 520)

            Spacer(minLength: 12)

            helpLink

            Spacer(minLength: 16)

            primaryButton("连接新设备") {
                withAnimation(.easeInOut(duration: 0.22)) {
                    viewModel.beginPairing()
                }
            }
            .disabled(viewModel.isStartingPairing)
        }
        .padding(22)
    }

    private var downloadCard: some View {
        Button {
            openExternalURL("https://www.wallpaperengine.io/android")
        } label: {
            HStack(spacing: 20) {
                VStack(spacing: 8) {
                    Image(systemName: "doc.and.arrow.down.fill")
                        .font(.title2)
                    Image(systemName: "iphone")
                        .font(.system(size: 42, weight: .regular))
                }
                .frame(width: 54)

                VStack(alignment: .leading, spacing: 6) {
                    Text("下载应用程序")
                        .font(.title2)
                    Text("需要 Android 10 或更高版本")
                        .font(.callout)
                }
            }
            .foregroundStyle(.white)
            .frame(width: 360, height: 104)
            .background(Color.accentColor, in: RoundedRectangle(cornerRadius: 6, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .stroke(Color.white.opacity(0.16), lineWidth: 1)
            }
        }
        .buttonStyle(.plain)
        .help("打开 Wallpaper Engine Android 下载页面")
    }

    private func pairingPage(pin: String) -> some View {
        VStack(spacing: 0) {
            pageTitle("移动设备")

            Text("在您的移动设备上输入此 PIN 以进行连接:")
                .font(.body)
                .padding(.top, 12)

            Spacer(minLength: 44)

            Text(pin)
                .font(.system(size: 64, weight: .light, design: .default))
                .monospacedDigit()
                .tracking(8)
                .accessibilityLabel(L("配对 PIN %@", pin))

            Spacer(minLength: 52)

            helpLink

            Spacer(minLength: 16)

            secondaryButton("取消") {
                withAnimation(.easeInOut(duration: 0.22)) {
                    viewModel.cancelPairing()
                }
            }
        }
        .padding(22)
    }

    private var devicesPage: some View {
        VStack(spacing: 0) {
            pageTitle("移动设备")

            Text("右键单击兼容的壁纸并选择“发送到移动设备”，以将壁纸无线传输到您的手机或平板电脑。")
                .font(.body)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 520)
                .padding(.top, 12)

            ScrollView {
                LazyVStack(spacing: 0) {
                    ForEach(viewModel.devices) { device in
                        deviceRow(device)
                    }
                }
                .padding(.top, 18)
            }
            .scrollIndicators(.hidden)
            .frame(maxWidth: 560)

            Spacer(minLength: 10)

            helpLink

            Spacer(minLength: 14)

            primaryButton("连接新设备") {
                withAnimation(.easeInOut(duration: 0.22)) {
                    viewModel.beginPairing()
                }
            }
            .disabled(viewModel.isStartingPairing)
        }
        .padding(22)
    }

    private func deviceRow(_ device: MobileDevice) -> some View {
        HStack(spacing: 16) {
            VStack(alignment: .leading, spacing: 5) {
                Text(device.name)
                    .font(.title2)
                    .lineLimit(1)
                Text(device.isConnected ? "已连接" : "未连接")
                    .font(.callout)
                    .foregroundStyle(device.isConnected ? Color.accentColor : .secondary)
            }

            Spacer(minLength: 12)

            if device.isConnected {
                HStack(spacing: 8) {
                    Image(systemName: "wifi")
                    Image(systemName: "lock.fill")
                }
                .font(.title2)
                .foregroundStyle(.green)
                .frame(width: 62)
            }

            Button {
                viewModel.removeDevice(id: device.id)
            } label: {
                Image(systemName: "trash.fill")
                    .font(.body)
                    .foregroundStyle(.white)
                    .frame(width: 38, height: 32)
                    .background(Color.red, in: RoundedRectangle(cornerRadius: 5, style: .continuous))
            }
            .buttonStyle(.plain)
            .help("删除此移动设备")
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 10)
        .frame(maxWidth: .infinity)
    }

    private var helpLink: some View {
        Button {
            openExternalURL("https://help.wallpaperengine.io/mobile")
        } label: {
            HStack(spacing: 7) {
                Text("访问我们的网站以获取更多帮助:")
                    .foregroundStyle(.secondary)
                Text("移动设备版 Wallpaper Engine 帮助")
                    .foregroundStyle(.tint)
            }
            .font(.callout)
            .multilineTextAlignment(.center)
        }
        .buttonStyle(.plain)
        .underline(false)
    }

    private func pageTitle(_ title: LocalizedStringKey) -> some View {
        Text(title)
            .font(.largeTitle)
            .multilineTextAlignment(.center)
    }

    private var closeButton: some View {
        Button {
            dismiss()
        } label: {
            Image(systemName: "xmark")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(.secondary)
                .frame(width: 26, height: 26)
                .background(.quaternary, in: Circle())
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .help("关闭")
        .keyboardShortcut(.cancelAction)
        .accessibilityLabel("关闭移动设备窗口")
    }

    private func primaryButton(_ title: LocalizedStringKey, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .frame(maxWidth: .infinity)
        }
        .buttonStyle(.borderedProminent)
        .controlSize(.large)
        .keyboardShortcut(.defaultAction)
    }

    private func secondaryButton(_ title: LocalizedStringKey, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .frame(maxWidth: .infinity)
        }
        .buttonStyle(.bordered)
        .controlSize(.large)
        .keyboardShortcut(.cancelAction)
    }

    private func openExternalURL(_ string: String) {
        guard let url = URL(string: string) else { return }
        NSWorkspace.shared.open(url)
    }
}

struct MobileDevicesView_Previews: PreviewProvider {
    static var previews: some View {
        Group {
            MobileDevicesView()
                .previewDisplayName("Welcome")
            MobileDevicesView(initialScreen: .pairing(pin: "8559"))
                .previewDisplayName("Pairing")
            MobileDevicesView(initialScreen: .devices, devices: [.preview])
                .previewDisplayName("Connected device")
        }
        .frame(width: 560, height: 460)
    }
}
