//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct SteamSetupView: View {
    @ObservedObject var viewModel: SteamSetupViewModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Spacer()
                Button("关闭") {
                    dismiss()
                }
                .buttonStyle(.bordered)
            }
            .padding(.horizontal, 16)
            .padding(.top, 12)

            stepIndicator
                .padding(.top, 8)
                .padding(.bottom, 16)

            Divider()

            Group {
                ScrollView {
                    Group {
                        switch viewModel.currentStep {
                        case 0:
                            welcomeStep
                        case 1:
                            SteamCMDStep(viewModel: viewModel)
                        case 2:
                            SteamLoginStep(viewModel: viewModel)
                        case 3:
                            completeStep
                        default:
                            EmptyView()
                        }
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 20)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .transition(.asymmetric(
                insertion: .move(edge: .trailing).combined(with: .opacity),
                removal: .move(edge: .leading).combined(with: .opacity)
            ))
            .animation(.spring(response: 0.4, dampingFraction: 0.85), value: viewModel.currentStep)

            Divider()

            HStack {
                if viewModel.currentStep > 0 {
                    Button {
                        withAnimation(.spring(response: 0.4, dampingFraction: 0.85)) {
                            viewModel.previousStep()
                        }
                    } label: {
                        Label("上一步", systemImage: "chevron.left")
                    }
                }

                Spacer()

                if viewModel.currentStep == viewModel.totalSteps - 1 {
                    Button {
                        viewModel.completeSetup()
                        dismiss()
                    } label: {
                        Text("完成")
                            .frame(width: 60)
                    }
                    .buttonStyle(.borderedProminent)
                } else {
                    Button {
                        withAnimation(.spring(response: 0.4, dampingFraction: 0.85)) {
                            viewModel.nextStep()
                        }
                    } label: {
                        HStack {
                            Text("下一步")
                            Image(systemName: "chevron.right")
                        }
                        .frame(width: 80)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!viewModel.canProceed)
                }
            }
            .padding(16)
        }
    }

    // MARK: - Step Indicator

    var stepIndicator: some View {
        HStack(spacing: 0) {
            ForEach(0..<viewModel.totalSteps, id: \.self) { step in
                ZStack {
                    Circle()
                        .fill(step <= viewModel.currentStep ? Color.accentColor : Color.secondary.opacity(0.3))
                        .frame(width: 28, height: 28)
                        .animation(.spring(response: 0.3), value: viewModel.currentStep)

                    if step < viewModel.currentStep {
                        Image(systemName: "checkmark")
                            .font(.caption)
                            .bold()
                            .foregroundStyle(.white)
                            .transition(.scale.combined(with: .opacity))
                    } else {
                        Text("\(step + 1)")
                            .font(.caption)
                            .bold()
                            .foregroundStyle(step == viewModel.currentStep ? .white : .secondary)
                    }
                }

                if step < viewModel.totalSteps - 1 {
                    Rectangle()
                        .fill(step < viewModel.currentStep ? Color.accentColor : Color.secondary.opacity(0.3))
                        .frame(height: 2)
                        .animation(.spring(response: 0.4).delay(0.1), value: viewModel.currentStep)
                }
            }
        }
        .padding(.horizontal, 40)
    }

    // MARK: - Welcome Step

    var welcomeStep: some View {
        VStack(spacing: 20) {
            Spacer()

            Image(systemName: "cloud.fill")
                .font(.system(size: 56))
                .foregroundStyle(
                    LinearGradient(
                        colors: [.blue, .cyan],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )
                .shadow(color: .blue.opacity(0.3), radius: 10)

            Text("设置 Steam 创意工坊")
                .font(.title)
                .bold()

            Text("连接 Steam 创意工坊，浏览并下载数十万 Wallpaper Engine 壁纸。")
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 40)

            HStack(spacing: 8) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                Text("需要在 Steam 上拥有 Wallpaper Engine 才能下载创意工坊壁纸")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .padding(10)
            .background(Color.orange.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(Color.orange.opacity(0.2), lineWidth: 1))
            .padding(.horizontal, 40)

            VStack(alignment: .leading, spacing: 5) {
                Label("中国大陆网络说明", systemImage: "network.badge.shield.half.filled")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text("此功能依赖全球 Steam 的 Web API、SteamCMD 登录服务和内容 CDN。蒸汽平台兼容性不保证；网络线路只能改善个别环节，Mirage 不承诺任何线路一定能解决登录或下载问题。")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(10)
            .background(Color.secondary.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .padding(.horizontal, 40)

            VStack(alignment: .leading, spacing: 14) {
                FeatureRow(icon: "magnifyingglass", color: .blue,
                           title: "搜索浏览", text: "搜索、筛选、排序海量壁纸")
                FeatureRow(icon: "arrow.down.circle", color: .green,
                           title: "一键下载", text: "通过 SteamCMD 直接下载到本地")
                FeatureRow(icon: "paintbrush", color: .purple,
                           title: "即刻使用", text: "下载完成自动加入壁纸库")
            }
            .padding(.horizontal, 50)

            Spacer()
        }
        .padding()
    }

    // MARK: - Complete Step

    var completeStep: some View {
        VStack(spacing: 20) {
            Spacer()

            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 64))
                .foregroundStyle(.green)
                .symbolEffect(.bounce, value: viewModel.currentStep == 3)

            Text("设置完成！")
                .font(.title)
                .bold()

            Text("Steam 登录已完成。Wallpaper Engine 所有权与项目访问权限将在首次下载时由 Steam 验证。")
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)

            VStack(alignment: .leading, spacing: 8) {
                if let path = viewModel.steamCMDPath {
                    HStack {
                        Image(systemName: "terminal.fill")
                            .foregroundStyle(.green)
                        Text("SteamCMD: \(path.lastPathComponent)")
                            .font(.caption)
                    }
                }
                if !viewModel.username.isEmpty {
                    HStack {
                        Image(systemName: "person.fill")
                            .foregroundStyle(.blue)
                        Text("账号: \(viewModel.username)")
                            .font(.caption)
                    }
                }
            }
            .padding()
            .background(Color(nsColor: NSColor.controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 8))

            Spacer()
        }
        .padding()
    }
}

struct FeatureRow: View {
    var icon: String
    var color: Color
    var title: LocalizedStringKey
    var text: LocalizedStringKey

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .frame(width: 32, height: 32)
                .font(.title3)
                .foregroundStyle(color)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.callout)
                    .bold()
                Text(text)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
        }
    }
}
