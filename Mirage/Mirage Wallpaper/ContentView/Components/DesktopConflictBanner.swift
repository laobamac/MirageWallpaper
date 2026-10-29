import SwiftUI

/// Explain a verified image change without guessing the other app or stopping it.
struct DesktopConflictBanner: View {
    @Bindable var wallpaperViewModel: WallpaperViewModel
    let isActive: Bool
    @State private var changed: Set<UInt32> = []
    @State private var dismissed: Set<UInt32> = []

    private var displays: [DisplayInfo] {
        wallpaperViewModel.connectedDisplays.filter { changed.contains($0.displayID) && !dismissed.contains($0.displayID) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(displays) { display in
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(L("%@ 的系统壁纸已被更换", display.name)).font(.caption.bold())
                        Text(L("退出 Mirage 时会保留这次更换。若两款动态壁纸互相覆盖，请从各自菜单中停止其中一款。"))
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button(L("知道了")) { dismissed.insert(display.displayID) }
                }
                .padding(8)
                .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 8))
            }
        }
        .task(id: isActive) {
            guard isActive else { return }
            while !Task.isCancelled {
                changed = DesktopOverrideService.shared.externallyChangedDisplays()
                do { try await Task.sleep(for: .seconds(15)) } catch { return }
            }
        }
    }
}
