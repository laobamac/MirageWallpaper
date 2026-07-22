//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Cocoa
import SwiftUI

protocol SettingsPage: View {
    var viewModel: GlobalSettingsViewModel { get set }

    init(globalSettings: GlobalSettingsViewModel)
}

private struct SettingsSection: Identifiable {
    let id: Int
    let title: String
    let systemImage: String
}

struct SettingsView: View {
    @EnvironmentObject var viewModel: GlobalSettingsViewModel
    @ObservedObject private var localization = MirageLocalization.shared

    private var sections: [SettingsSection] {
        [
            .init(id: 0, title: L("性能"), systemImage: "speedometer"),
            .init(id: 1, title: L("通用"), systemImage: "gearshape"),
            .init(id: 2, title: L("插件"), systemImage: "puzzlepiece.extension"),
            .init(id: 3, title: L("屏保"), systemImage: "sparkles.tv"),
            .init(id: 4, title: L("关于"), systemImage: "person.3"),
        ]
    }

    // Compares against an in-memory snapshot of the last-saved settings instead
    // of decoding GlobalSettings JSON from UserDefaults on every footer render.
    private var hasUnsavedChanges: Bool {
        viewModel.settings != viewModel.savedSettings
    }

    var body: some View {
        VStack(spacing: 0) {
            header

            Divider()

            Group {
                switch viewModel.selection {
                case 0:
                    PerformancePage(globalSettings: viewModel)
                case 1:
                    GeneralPage(globalSettings: viewModel)
                case 2:
                    PluginsPage(globalSettings: viewModel)
                case 3:
                    ScreenSaverPage(globalSettings: viewModel)
                case 4:
                    AboutUsView()
                default:
                    PerformancePage(globalSettings: viewModel)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            Divider()

            footer
        }
        .frame(minWidth: 720, idealWidth: 780, minHeight: 520, idealHeight: 620)
        .environment(\.locale, localization.locale)
    }

    private var header: some View {
        HStack(spacing: 4) {
            Spacer(minLength: 0)
            ForEach(sections) { section in
                SettingsTab(
                    title: section.title,
                    systemImage: section.systemImage,
                    isSelected: viewModel.selection == section.id
                ) {
                    viewModel.selection = section.id
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 16)
        .padding(.top, 12)
        .padding(.bottom, 8)
        .background(.bar)
    }

    private var footer: some View {
        HStack {
            if hasUnsavedChanges {
                Image(systemName: "exclamationmark.circle.fill")
                    .foregroundStyle(.yellow)
                Text("已修改")
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                viewModel.isSettingsPresented = false
            } label: {
                Text("取消").frame(width: 50)
            }
            .keyboardShortcut(.cancelAction)
            Button {
                viewModel.save()
                viewModel.isSettingsPresented = false
            } label: {
                Text("好").frame(width: 50)
            }
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
        }
        .padding(20)
    }
}

/// A single preference-style tab: a large icon with its label underneath,
/// highlighted when selected — mirrors the native macOS toolbar look.
private struct SettingsTab: View {
    let title: String
    let systemImage: String
    let isSelected: Bool
    let action: () -> Void

    @State private var isHovering = false

    var body: some View {
        Button(action: action) {
            VStack(spacing: 4) {
                Image(systemName: systemImage)
                    .font(.system(size: 20, weight: .regular))
                    .frame(height: 24)
                Text(title)
                    .font(.system(size: 11))
                    .lineLimit(1)
            }
            .foregroundStyle(isSelected ? Color.accentColor : Color.primary)
            .frame(width: 74)
            .padding(.vertical, 6)
            .background(
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(background)
            )
            .contentShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
        }
        .buttonStyle(.plain)
        .onHover { isHovering = $0 }
    }

    private var background: Color {
        if isSelected {
            return Color.accentColor.opacity(0.15)
        }
        return isHovering ? Color.primary.opacity(0.08) : Color.clear
    }
}

struct SettingsView_Previews: PreviewProvider {
    static var previews: some View {
        SettingsView()
            .environmentObject({ () -> GlobalSettingsViewModel in
                let viewModel = GlobalSettingsViewModel()
                viewModel.selection = 2
                return viewModel
            }())
            .frame(width: 780, height: 620)
    }
}
