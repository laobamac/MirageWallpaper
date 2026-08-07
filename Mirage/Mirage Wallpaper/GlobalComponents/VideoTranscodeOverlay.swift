//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

/// Floating card shown over the main window while a video wallpaper is being
/// rewritten to H.264. Non-modal on purpose: the conversion runs in the renderer
/// process, so nothing in the app needs to be blocked while it finishes.
struct VideoTranscodeOverlay: View {
    @ObservedObject private var model = VideoTranscodeProgressModel.shared
    @ObservedObject private var localization = MirageLocalization.shared

    var body: some View {
        if !model.jobs.isEmpty {
            VStack(alignment: .leading, spacing: 14) {
                HStack(spacing: 10) {
                    ProgressView()
                        .controlSize(.small)
                    Text(L("正在转换视频格式"))
                        .font(.headline)
                }

                Text(L("此壁纸使用 macOS 无法直接解码的编码，正在转换为 H.264。转换只需进行一次。"))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                ForEach(model.jobs) { job in
                    VStack(alignment: .leading, spacing: 5) {
                        HStack(alignment: .firstTextBaseline) {
                            Text(job.title)
                                .font(.subheadline)
                                .lineLimit(1)
                                .truncationMode(.middle)
                            Spacer(minLength: 12)
                            Text("\(Int((job.progress * 100).rounded()))%")
                                .font(.subheadline.monospacedDigit())
                                .foregroundStyle(.secondary)
                        }
                        ProgressView(value: job.progress)
                            .progressViewStyle(.linear)
                        if model.jobs.count > 1 {
                            Text(L("显示器 %d", job.id + 1))
                                .font(.caption)
                                .foregroundStyle(.tertiary)
                        }
                    }
                }
            }
            .padding(20)
            .frame(width: 380)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(Color.primary.opacity(0.09))
            )
            .shadow(color: .black.opacity(0.28), radius: 22, y: 10)
            .padding(24)
            .transition(.opacity.combined(with: .scale(scale: 0.96)))
            .animation(.easeInOut(duration: 0.22), value: model.jobs)
            .environment(\.locale, localization.locale)
        }
    }
}
