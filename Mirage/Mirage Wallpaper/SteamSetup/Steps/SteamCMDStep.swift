//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct SteamCMDStep: View {
    @ObservedObject var viewModel: SteamSetupViewModel

    var body: some View {
        VStack(spacing: 20) {
            Spacer()

            Group {
                switch viewModel.steamCMDInstallState {
                case .detecting:
                    VStack(spacing: 16) {
                        ProgressView()
                            .scaleEffect(1.5)
                        Text("正在检测 SteamCMD...")
                            .font(.title3)
                            .foregroundStyle(.secondary)
                    }

                case .rosettaRequired:
                    VStack(spacing: 16) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .font(.system(size: 48))
                            .foregroundStyle(.orange)

                        Text(L("需要安装 Rosetta 2"))
                            .font(.title2)
                            .bold()

                        Text(L("当前 Apple 芯片 Mac 尚未安装 Rosetta 2。请先通过 macOS 系统方式安装 Rosetta 2，再重新打开此设置页面。"))
                            .font(.body)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                            .padding(.horizontal, 30)
                    }

                case .found(let path):
                    VStack(spacing: 16) {
                        Image(systemName: "checkmark.circle.fill")
                            .font(.system(size: 56))
                            .foregroundStyle(.green)
                            .transition(.scale.combined(with: .opacity))

                        Text("已找到 SteamCMD")
                            .font(.title2)
                            .bold()

                        Text(path)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .padding(.horizontal, 12)
                            .padding(.vertical, 6)
                            .background(Color(nsColor: NSColor.controlBackgroundColor))
                            .clipShape(RoundedRectangle(cornerRadius: 6))
                    }

                case .notFound:
                    VStack(spacing: 16) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .font(.system(size: 48))
                            .foregroundStyle(.orange)

                        Text("未找到 SteamCMD")
                            .font(.title2)
                            .bold()

                        Text("SteamCMD 是 Valve 的官方命令行工具，用于下载 Steam 创意工坊内容。\nMirage 会下载官方 bootstrap，随后由 SteamCMD 完成首次更新；所需时间和空间取决于网络。")
                            .font(.body)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                            .padding(.horizontal, 30)

                        Button {
                            viewModel.installSteamCMD()
                        } label: {
                            Label("安装 SteamCMD", systemImage: "arrow.down.app.fill")
                                .frame(width: 160)
                        }
                        .buttonStyle(.borderedProminent)
                    }

                case .downloading(let progress):
                    VStack(spacing: 16) {
                        ProgressView(value: progress)
                            .frame(width: 200)
                            .animation(.linear, value: progress)

                        Text("正在下载 SteamCMD...")
                            .font(.body)
                            .foregroundStyle(.secondary)

                        Text("\(Int(progress * 100))%")
                            .font(.title3)
                            .bold()
                            .foregroundStyle(.blue)

                        Button("取消下载") {
                            viewModel.cancelSteamCMDInstallation()
                        }
                        .buttonStyle(.bordered)
                    }

                case .extracting:
                    VStack(spacing: 16) {
                        ProgressView()
                            .scaleEffect(1.5)

                        Text("正在解压...")
                            .font(.body)
                            .foregroundStyle(.secondary)
                    }

                case .initializing:
                    VStack(spacing: 16) {
                        ProgressView()
                            .scaleEffect(1.5)

                        Text("正在完成 SteamCMD 首次初始化...")
                            .font(.body)
                            .foregroundStyle(.secondary)

                        Text("这一步需要连接 Steam 服务，请勿关闭 Mirage。")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                case .installed(let path):
                    VStack(spacing: 16) {
                        Image(systemName: "checkmark.circle.fill")
                            .font(.system(size: 56))
                            .foregroundStyle(.green)
                            .transition(.scale.combined(with: .opacity))

                        Text("安装完成")
                            .font(.title2)
                            .bold()

                        Text(path)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .padding(.horizontal, 12)
                            .padding(.vertical, 6)
                            .background(Color(nsColor: NSColor.controlBackgroundColor))
                            .clipShape(RoundedRectangle(cornerRadius: 6))
                    }

                case .failed(let message):
                    VStack(spacing: 16) {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 48))
                            .foregroundStyle(.red)

                        Text("安装失败")
                            .font(.title2)
                            .bold()

                        Text(message)
                            .font(.caption)
                            .foregroundStyle(.red)
                            .multilineTextAlignment(.center)
                            .padding(.horizontal, 20)

                        Button {
                            viewModel.installSteamCMD()
                        } label: {
                            Label("重试", systemImage: "arrow.clockwise")
                        }
                        .buttonStyle(.borderedProminent)
                    }
                }
            }
            .animation(.spring(response: 0.4, dampingFraction: 0.85), value: viewModel.steamCMDInstallState)

            Spacer()
        }
        .padding()
        .onAppear {
            viewModel.detectSteamCMD()
        }
    }
}
