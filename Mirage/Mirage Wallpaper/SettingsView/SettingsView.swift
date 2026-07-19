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

    private var hasUnsavedChanges: Bool {
        guard let data = UserDefaults.standard.data(forKey: "GlobalSettings"),
              let savedSettings = try? JSONDecoder().decode(GlobalSettings.self, from: data) else {
            return false
        }
        return viewModel.settings != savedSettings
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
        HStack(spacing: 6) {
            Text(L("设置"))
                .font(.headline)
                .padding(.trailing, 8)

            Picker("", selection: $viewModel.selection) {
                ForEach(sections) { section in
                    Label(section.title, systemImage: section.systemImage)
                        .tag(section.id)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            Spacer(minLength: 0)
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 12)
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
