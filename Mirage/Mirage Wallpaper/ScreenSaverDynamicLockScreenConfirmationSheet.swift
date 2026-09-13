//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct ScreenSaverDynamicLockScreenConfirmationSheet: View {
    @ObservedObject var manager: ScreenSaverDynamicLockScreenManager
    @State private var input = ""
    @State private var showingError = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Label(LocalizedStringKey("屏保动态锁屏"), systemImage: "lock.rectangle")
                .font(.title2.weight(.semibold))
            Text(LocalizedStringKey("此功能通过系统屏保和系统墙纸配置实现，支持 macOS 14.2 及以上版本。它依赖未公开的系统配置格式，可能随 macOS 更新失效，并会在锁屏期间重启系统墙纸服务。启用前请确认你了解并接受这一风险。"))
                .fixedSize(horizontal: false, vertical: true)
                .foregroundStyle(.secondary)
            TextField("输入“我同意”或“Agree”", text: $input)
                .textFieldStyle(.roundedBorder)
            if showingError {
                Text("请输入“我同意”或“Agree”")
                    .foregroundStyle(.red)
                    .font(.caption)
            }
            HStack {
                Spacer()
                Button("取消") { manager.isConfirmationPresented = false }
                Button("确认开启") {
                    showingError = !manager.confirmAndEnable(input: input)
                }
                .buttonStyle(.borderedProminent)
                .disabled(input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
        }
        .padding(24)
        .frame(width: 560)
    }
}
