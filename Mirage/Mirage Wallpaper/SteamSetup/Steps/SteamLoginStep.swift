//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import AppKit

struct SteamLoginStep: View {
    @ObservedObject var viewModel: SteamSetupViewModel

    @State private var isPasswordVisible = false
    @State private var showLog = false

    var body: some View {
        VStack(spacing: 20) {
            Spacer()

            Image(systemName: "person.crop.circle.fill")
                .font(.system(size: 48))
                .foregroundStyle(
                    LinearGradient(
                        colors: [.blue, .indigo],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )

            Text("登录 Steam 账号")
                .font(.title2)
                .bold()

            Text("需要一个拥有 Wallpaper Engine 的全球 Steam 账号来下载创意工坊内容。")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 40)

            VStack(alignment: .leading, spacing: 6) {
                Label("Mirage 并非 Steam 官方客户端。", systemImage: "info.circle.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
                Text("若继续登录，密码仅通过本机的 Valve SteamCMD 安全终端提交，不会写入命令行或 Mirage 日志。SteamCMD 会在本机保存会话以便下载；您可随时在创意工坊页面“退出登录”清除它。")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(10)
            .frame(maxWidth: 420, alignment: .leading)
            .background(Color.orange.opacity(0.08))
            .clipShape(RoundedRectangle(cornerRadius: 8))

            VStack(spacing: 12) {
                switch viewModel.loginState {
                case .idle, .failed:
                    loginForm
                case .loggingIn:
                    loggingInView
                case .waitingForGuard(let guardType):
                    if guardType == .mobileConfirm {
                        mobileConfirmView
                    } else {
                        guardCodeView(guardType)
                    }
                case .success:
                    successView
                }
            }
            .frame(width: 300)
            .animation(.spring(response: 0.4, dampingFraction: 0.85), value: viewModel.loginState)

            if let error = viewModel.errorMessage {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 30)
                    .transition(.opacity)
            }

            if viewModel.loginState != .idle {
                VStack(spacing: 4) {
                    HStack(spacing: 10) {
                        Button {
                            showLog.toggle()
                        } label: {
                            Label(LocalizedStringKey(showLog ? "隐藏日志" : "显示 SteamCMD 日志"), systemImage: "terminal")
                                .font(.caption2)
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(.secondary)

                        Button("复制脱敏日志") {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(viewModel.loginLog.joined(separator: "\n"), forType: .string)
                        }
                        .buttonStyle(.plain)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                    }

                    if showLog {
                        ScrollView {
                            ScrollViewReader { proxy in
                                VStack(alignment: .leading, spacing: 1) {
                                    ForEach(Array(viewModel.loginLog.enumerated()), id: \.offset) { idx, line in
                                        Text(line)
                                            .font(.system(size: 10, design: .monospaced))
                                            .foregroundStyle(.green)
                                            .id(idx)
                                    }
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .onChange(of: viewModel.loginLog.count) { _, _ in
                                    if let last = viewModel.loginLog.indices.last {
                                        proxy.scrollTo(last, anchor: .bottom)
                                    }
                                }
                            }
                        }
                        .frame(width: 400, height: 100)
                        .padding(6)
                        .background(Color.black.opacity(0.85))
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                    }
                }
            }

            Spacer()
        }
        .padding()
    }

    var loginForm: some View {
        VStack(spacing: 10) {
            if let savedUsername = viewModel.reusableSessionUsername {
                Button {
                    viewModel.useSavedSession()
                } label: {
                    Label("使用已保存会话：\(savedUsername)", systemImage: "person.crop.circle.badge.checkmark")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)

                HStack {
                    Divider()
                    Text("或使用密码登录")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                    Divider()
                }
            }

            HStack {
                Image(systemName: "person.fill")
                    .foregroundStyle(.secondary)
                    .frame(width: 20)
                TextField("全球 Steam 登录账户名（非昵称）", text: $viewModel.username)
                    .textFieldStyle(.roundedBorder)
            }

            HStack {
                Image(systemName: "lock.fill")
                    .foregroundStyle(.secondary)
                    .frame(width: 20)
                if isPasswordVisible {
                    TextField("密码", text: $viewModel.password)
                        .textFieldStyle(.roundedBorder)
                } else {
                    SecureField("密码", text: $viewModel.password)
                        .textFieldStyle(.roundedBorder)
                }
                Button {
                    isPasswordVisible.toggle()
                } label: {
                    Image(systemName: isPasswordVisible ? "eye.slash" : "eye")
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
            }

            Button {
                viewModel.login()
            } label: {
                Label("登录", systemImage: "arrow.right.circle.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(viewModel.username.isEmpty || viewModel.password.isEmpty)
            .padding(.top, 4)
        }
    }

    var loggingInView: some View {
        VStack(spacing: 16) {
            ProgressView()
                .scaleEffect(1.5)
            Text("正在登录...")
                .foregroundStyle(.secondary)
            Button("取消登录") {
                viewModel.cancelLogin()
            }
            .buttonStyle(.bordered)
        }
    }

    func guardCodeView(_ type: SteamGuardType) -> some View {
        VStack(spacing: 12) {
            Image(systemName: type == .email ? "envelope.badge.shield.half.filled.fill" : "iphone")
                .font(.system(size: 36))
                .foregroundStyle(.blue)

            Text(LocalizedStringKey(type == .email ? "请输入邮箱验证码" : "请输入手机验证码"))
                .font(.callout)
                .bold()

            Text(type == .email ?
                 "验证码已发送到您的邮箱" :
                 "请查看 Steam 手机令牌")
                .font(.caption)
                .foregroundStyle(.secondary)

            HStack {
                Image(systemName: "shield.fill")
                    .foregroundStyle(.secondary)
                    .frame(width: 20)
                TextField("验证码", text: $viewModel.guardCode)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 150)
            }

            Button {
                viewModel.submitGuardCode()
            } label: {
                Label("验证", systemImage: "checkmark.circle.fill")
                    .frame(width: 100)
            }
            .buttonStyle(.borderedProminent)
            .disabled(viewModel.guardCode.isEmpty)

            Button("取消登录") {
                viewModel.cancelLogin()
            }
            .buttonStyle(.bordered)
        }
    }

    var successView: some View {
        VStack(spacing: 12) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 48))
                .foregroundStyle(.green)
                .transition(.scale.combined(with: .opacity))

            Text("登录成功")
                .font(.title3)
                .bold()

            HStack {
                Image(systemName: "person.fill")
                    .foregroundStyle(.green)
                Text(viewModel.username)
            }
            .font(.callout)
        }
    }

    var mobileConfirmView: some View {
        VStack(spacing: 16) {
            Image(systemName: "iphone.radiowaves.left.and.right")
                .font(.system(size: 48))
                .foregroundStyle(.blue)
                .symbolEffect(.pulse, options: .repeating)

            Text("请在手机上确认登录")
                .font(.title3)
                .bold()

            Text("打开 Steam 手机应用，在通知中确认此次登录请求")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 20)

            HStack(spacing: 8) {
                ProgressView()
                    .scaleEffect(0.8)
                Text(viewModel.guardWaitElapsed > 0
                     ? "等待确认中… \(viewModel.guardWaitElapsed) 秒"
                     : "等待确认中…")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            .padding(.top, 8)

            Button("取消登录") {
                viewModel.cancelLogin()
            }
            .buttonStyle(.bordered)
        }
    }
}
