//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct PluginsPage: SettingsPage {
    @Bindable var viewModel: GlobalSettingsViewModel
    
    init(globalSettings viewModel: GlobalSettingsViewModel) {
        self.viewModel = viewModel
    }

    var body: some View {
        Form {
            Section {
                Text("暂无内置插件。")
                    .foregroundStyle(.secondary)
            } header: {
                Label("内置", systemImage: "square.dashed.inset.filled")
            }
            Section {
                Text("无")
                    .foregroundStyle(.secondary)
            } header: {
                Label("第三方", systemImage: "person.3.fill")
            }
        }
        .formStyle(.grouped)
    }
}

struct PluginPage_Previews: PreviewProvider {
    static var previews: some View {
        SettingsView()
            .environment({ () -> GlobalSettingsViewModel in
                let viewModel = GlobalSettingsViewModel()
                viewModel.selection = 2
                return viewModel
            }())
            .frame(width: 500, height: 600)
    }
}
