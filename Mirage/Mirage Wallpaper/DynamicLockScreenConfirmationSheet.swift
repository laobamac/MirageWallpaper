//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct DynamicLockScreenConfirmationSheet: View {
    @ObservedObject var manager: DynamicLockScreenManager
    @State private var input = ""
    @State private var showingError = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Label("动态锁屏", systemImage: "lock.rectangle")
                .font(.title2.weight(.semibold))
            Text("此功能仅支持 macOS 26 及以上版本。Mirage 使用的动态锁屏 API 来自逆向分析，可能随系统更新失效，也可能导致系统壁纸服务重启。启用前请确认你了解并接受这一风险。")
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
        .frame(width: 520)
    }
}
