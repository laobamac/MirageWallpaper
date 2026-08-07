//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct TopTabBar: SubviewOfContentView {
    @ObservedObject var viewModel: ContentViewModel
    @ObservedObject var wallpaperViewModel: WallpaperViewModel

    init(contentViewModel viewModel: ContentViewModel, wallpaperViewModel: WallpaperViewModel) {
        self.viewModel = viewModel
        self.wallpaperViewModel = wallpaperViewModel
    }

    private var downloadCount: Int {
        AppDelegate.shared.workshopViewModel.activeDownloadCount
    }

    var body: some View {
        HStack(spacing: 10) {
            HStack(spacing: 4) {
                tab(index: 0, title: "已安装", systemImage: "square.and.arrow.down.fill")
                tab(index: 1, title: "发现", systemImage: "sparkle.magnifyingglass")
                tab(index: 2, title: "创意工坊", systemImage: "cloud.fill", badge: downloadCount)
                tab(index: 3, title: "小组件", systemImage: "square.grid.2x2.fill")
            }
            .padding(4)
            .mirageGlass(in: Capsule(), fallback: AnyShapeStyle(.quaternary.opacity(0.6)), interactive: false)
            .fixedSize()

            Spacer(minLength: 10)

            HStack(spacing: 2) {
                chromeButton(title: "移动端", systemImage: "platter.filled.bottom.iphone") { }
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
    private func tab(index: Int, title: LocalizedStringKey, systemImage: String, badge: Int = 0) -> some View {
        let isSelected = viewModel.topTabBarSelection == index
        let isHovering = viewModel.topTabBarHoverSelection == index

        Button {
            viewModel.topTabBarSelection = index
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
        .onHover { viewModel.topTabBarHoverSelection = $0 ? index : -1 }
    }

    @ViewBuilder
    private func chromeButton(title: LocalizedStringKey, systemImage: String, action: @escaping () -> Void) -> some View {
        ChromeButton(title: title, systemImage: systemImage, action: action)
    }
}

// A borderless chrome button with a subtle rounded hover highlight, replacing
// the old hard `Divider`-separated plain buttons.
private struct ChromeButton: View {
    let title: LocalizedStringKey
    let systemImage: String
    let action: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .background(
                    RoundedRectangle(cornerRadius: 7, style: .continuous)
                        .fill(hovering ? Color.primary.opacity(0.08) : Color.clear)
                )
                .contentShape(RoundedRectangle(cornerRadius: 7, style: .continuous))
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
    }
}
