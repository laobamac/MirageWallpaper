//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct TopTabBar: View {
    @ObservedObject var navigationModel: MainNavigationModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel
    @ObservedObject var mobileDevicesViewModel: MobileDevicesViewModel
    @State private var hoverSelection: MainSection?

    init(
        navigationModel: MainNavigationModel,
        wallpaperViewModel: WallpaperViewModel,
        mobileDevicesViewModel: MobileDevicesViewModel = AppDelegate.shared.mobileDevicesViewModel
    ) {
        self.navigationModel = navigationModel
        self.wallpaperViewModel = wallpaperViewModel
        self.mobileDevicesViewModel = mobileDevicesViewModel
    }

    private var hasConnectedMobileDevice: Bool {
        mobileDevicesViewModel.devices.contains(where: \.isConnected)
    }

    var body: some View {
        HStack(spacing: 10) {
            HStack(spacing: 4) {
                tab(section: .installed, title: "已安装", systemImage: "square.and.arrow.down.fill")
                tab(section: .discover, title: "发现", systemImage: "sparkle.magnifyingglass")
                WorkshopActiveDownloadCount(
                    downloadStore: AppDelegate.shared.workshopViewModel.downloadStore
                ) { count in
                    tab(section: .workshop, title: "创意工坊", systemImage: "cloud.fill", badge: count)
                }
                tab(section: .subscriptions, title: "已订阅", systemImage: "checkmark.circle.fill")
            }
            .padding(4)
            .mirageGlass(in: Capsule(), fallback: AnyShapeStyle(.quaternary.opacity(0.6)), interactive: false)
            .fixedSize()

            Spacer(minLength: 10)

            HStack(spacing: 2) {
                chromeButton(
                    title: "移动端",
                    systemImage: "platter.filled.bottom.iphone",
                    iconColor: hasConnectedMobileDevice ? .green : nil
                ) {
                    navigationModel.isMobileDevicesPresented = true
                }
                DisplayPicker(wallpaperViewModel: wallpaperViewModel)
                chromeButton(title: "设置", systemImage: "gearshape.fill") {
                    AppDelegate.shared.openSettingsWindow()
                }
            }
            .padding(3)
            .mirageGlass(in: Capsule(), fallback: AnyShapeStyle(.quaternary.opacity(0.32)), interactive: false)
            .fixedSize()
        }
        .padding(.vertical, 2)
    }

    // A single segmented pill. The selected segment gets an accent-filled
    // capsule; hover gets a soft translucent capsule. No hard rectangles.
    @ViewBuilder
    private func tab(section: MainSection, title: LocalizedStringKey, systemImage: String, badge: Int = 0) -> some View {
        let isSelected = navigationModel.selection == section
        let isHovering = hoverSelection == section

        Button {
            navigationModel.selection = section
        } label: {
            Label(title, systemImage: systemImage)
                .font(.headline)
                .padding(.horizontal, 14)
                .padding(.vertical, 7)
                .foregroundStyle(isSelected ? Color.white : Color.primary)
                .background {
                    if isSelected {
                        Capsule().fill(Color.accentColor)
                    } else if isHovering {
                        Capsule().fill(Color.primary.opacity(0.08))
                    }
                }
                .overlay(alignment: .topTrailing) {
                    if badge > 0 {
                        Text("\(badge)")
                            .font(.system(size: 9).bold())
                            .monospacedDigit()
                            .foregroundStyle(.white)
                            .frame(minWidth: 14, minHeight: 14)
                            .background(Color.red, in: Capsule())
                            .offset(x: 2, y: -2)
                    }
                }
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .onHover { hoverSelection = $0 ? section : nil }
    }

    @ViewBuilder
    private func chromeButton(
        title: LocalizedStringKey,
        systemImage: String,
        iconColor: Color? = nil,
        action: @escaping () -> Void
    ) -> some View {
        ChromeButton(
            title: title,
            systemImage: systemImage,
            iconColor: iconColor,
            action: action
        )
    }
}

// A borderless chrome button with a subtle rounded hover highlight, replacing
// the old hard `Divider`-separated plain buttons.
private struct ChromeButton: View {
    let title: LocalizedStringKey
    let systemImage: String
    var iconColor: Color? = nil
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            HStack(spacing: 6) {
                Image(systemName: systemImage)
                    .foregroundStyle(iconColor ?? Color.primary)
                Text(title)
            }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(
                    RoundedRectangle(cornerRadius: 7, style: .continuous)
                        .fill(hovering ? Color.primary.opacity(0.08) : Color.clear)
                )
                .contentShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(title))
        .onHover { hovering = $0 }
    }
}
