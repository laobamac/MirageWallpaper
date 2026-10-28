//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import AppKit
import CoreImage
import CoreImage.CIFilterBuiltins

struct SteamLoginStep: View {
    @ObservedObject var viewModel: SteamSetupViewModel
    var editorLogin = false
    @State private var isPasswordVisible = false
    @State private var showPasswordLogin = false

    var body: some View {
        VStack(spacing: 18) {
            if !editorLogin {
                Image(systemName: "person.crop.circle.fill")
                    .font(.system(size: 44))
                    .foregroundStyle(.blue)

                Text("登录 Steam 账号")
                    .font(.title2)
                    .bold()

                Text("需要一个拥有 Wallpaper Engine 的全球 Steam 账号来下载创意工坊内容。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)

                VStack(alignment: .leading, spacing: 6) {
                    Label("Mirage 并非 Steam 官方客户端。", systemImage: "info.circle.fill")
                        .font(.caption)
                        .foregroundStyle(.orange)
                    Text("登录由内置 Steam 服务直接连接 Valve。二维码和密码均只在本机处理，刷新令牌保存在 macOS 钥匙串中，可随时退出并清除。")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(10)
                .frame(maxWidth: 420, alignment: .leading)
                .background(Color.orange.opacity(0.08))
                .clipShape(RoundedRectangle(cornerRadius: 8))

            }
            Group {
                switch viewModel.loginState {
                case .idle, .failed:
                    entryView
                case .loggingIn:
                    loggingInView
                case .waitingForQR(let challenge):
                    qrView(challenge)
                case .waitingForGuard(let guardType):
                    guardType == .mobileConfirm ? AnyView(mobileConfirmView) : AnyView(guardCodeView(guardType))
                case .success:
                    successView
                }
            }
            .frame(maxWidth: editorLogin ? .infinity : 320)
            .animation(.easeInOut(duration: 0.2), value: viewModel.loginState)

            if let error = viewModel.errorMessage {
                Text(error)
                    .font(.caption)
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 30)
            }
        }
        .padding(editorLogin ? 0 : 16)
    }

    private var entryView: some View {
        VStack(spacing: 10) {
            if let savedUsername = viewModel.reusableSessionUsername {
                Button {
                    viewModel.useSavedSession()
                } label: {
                    Label("使用已保存会话：\(savedUsername)", systemImage: "person.crop.circle.badge.checkmark")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
            }

            if showPasswordLogin {
                passwordForm
                Button("改用二维码登录") {
                    showPasswordLogin = false
                    viewModel.startQRLogin()
                }
                .buttonStyle(.link)
            } else {
                Button {
                    viewModel.startQRLogin()
                } label: {
                    Label("使用 Steam 手机应用扫码登录", systemImage: "qrcode")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)

                Button("改用账户密码登录") {
                    showPasswordLogin = true
                }
                .buttonStyle(.link)
            }
        }
    }

    private var passwordForm: some View {
        VStack(spacing: 10) {
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
                }
                .buttonStyle(.plain)
                .help(isPasswordVisible ? "隐藏密码" : "显示密码")
            }
            Button {
                viewModel.login()
            } label: {
                Label("登录", systemImage: "arrow.right.circle.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(viewModel.username.isEmpty || viewModel.password.isEmpty)
        }
    }

    private var loggingInView: some View {
        VStack(spacing: 14) {
            ProgressView()
                .controlSize(.large)
            Text("正在连接 Steam…")
                .foregroundStyle(.secondary)
            Button("取消登录") { viewModel.cancelLogin() }
                .buttonStyle(.bordered)
        }
    }

    private func qrView(_ challenge: String) -> some View {
        VStack(spacing: 12) {
            if let image = qrImage(challenge) {
                Image(nsImage: image)
                    .interpolation(.none)
                    .resizable()
                    .frame(width: editorLogin ? 156 : 196, height: editorLogin ? 156 : 196)
                    .padding(editorLogin ? 8 : 12)
                    .background(.white)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                    .id(challenge)
            }
            Text("使用 Steam 手机应用扫描二维码")
                .font(.callout)
                .bold()
            Text("在 Steam 手机应用中打开扫码功能并确认登录")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            HStack {
                Button {
                    viewModel.refreshQRCode()
                } label: {
                    Label("刷新二维码", systemImage: "arrow.clockwise")
                }
                Button("取消登录") { viewModel.cancelLogin() }
            }
            .buttonStyle(.bordered)
            Button("改用账户密码登录") {
                viewModel.cancelLogin()
                showPasswordLogin = true
            }
            .buttonStyle(.link)
        }
    }

    private func guardCodeView(_ type: SteamGuardType) -> some View {
        VStack(spacing: 12) {
            Image(systemName: type == .email ? "envelope.badge.shield.half.filled.fill" : "iphone")
                .font(.system(size: 36))
                .foregroundStyle(.blue)
            Text(LocalizedStringKey(type == .email ? "请输入邮箱验证码" : "请输入手机验证码"))
                .font(.callout)
                .bold()
            TextField("验证码", text: $viewModel.guardCode)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
            Button("验证") { viewModel.submitGuardCode() }
                .buttonStyle(.borderedProminent)
                .disabled(viewModel.guardCode.isEmpty)
            Button("取消登录") { viewModel.cancelLogin() }
                .buttonStyle(.bordered)
        }
    }

    private var mobileConfirmView: some View {
        VStack(spacing: 14) {
            Image(systemName: "iphone.radiowaves.left.and.right")
                .font(.system(size: 44))
                .foregroundStyle(.blue)
            Text("请在手机上确认登录")
                .font(.title3)
                .bold()
            Text("打开 Steam 手机应用，在通知中确认此次登录请求")
                .font(.caption)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            ProgressView()
            Text("等待确认中… \(viewModel.guardWaitElapsed) 秒")
                .font(.caption)
                .foregroundStyle(.secondary)
            Button("取消登录") { viewModel.cancelLogin() }
                .buttonStyle(.bordered)
        }
    }

    private var successView: some View {
        VStack(spacing: 12) {
            Image(systemName: "checkmark.circle.fill")
                .font(.system(size: 48))
                .foregroundStyle(.green)
            Text("登录成功")
                .font(.title3)
                .bold()
            Label(viewModel.username, systemImage: "person.fill")
                .font(.callout)
        }
    }

    private func qrImage(_ value: String) -> NSImage? {
        let filter = CIFilter.qrCodeGenerator()
        filter.message = Data(value.utf8)
        filter.correctionLevel = "M"
        guard let output = filter.outputImage?.transformed(by: CGAffineTransform(scaleX: 10, y: 10)) else { return nil }
        let representation = NSCIImageRep(ciImage: output)
        let image = NSImage(size: representation.size)
        image.addRepresentation(representation)
        return image
    }
}
