//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import SwiftUI

struct ProjectFeedbackBanner: View {
    var showsActions = true

    @State private var copied = false

    private let groupNumber = "2160040437"
    private let issuesURL = URL(string: "https://github.com/laobamac/MirageWallpaper/issues/new/choose")!

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            Image(systemName: "exclamationmark.bubble.fill")
                .font(.title2)
                .foregroundStyle(.orange)

            VStack(alignment: .leading, spacing: 3) {
                Text("Mirage 仍处于早期阶段")
                    .font(.headline)
                Text("遇到问题请认真撰写 Issue，或加入 QQ 交流群 \(groupNumber) 反馈。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            Spacer(minLength: 8)

            if showsActions {
                Button {
                    AppDelegate.shared.showAboutUs()
                } label: {
                    Label("支持 Mirage", systemImage: "heart")
                }
                .buttonStyle(.bordered)

                Link(destination: issuesURL) {
                    Label("提交 Issue", systemImage: "square.and.pencil")
                }
                .buttonStyle(.bordered)

                Button {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(groupNumber, forType: .string)
                    copied = true
                } label: {
                    Label(copied ? "群号已复制" : "复制群号", systemImage: copied ? "checkmark" : "doc.on.doc")
                }
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 9)
        .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 8))
        .overlay {
            RoundedRectangle(cornerRadius: 8)
                .stroke(Color.orange.opacity(0.45))
        }
    }
}
