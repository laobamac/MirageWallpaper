//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI
import AppKit

struct DirectWorkshopSettingsSection: View {
    @ObservedObject private var service = DirectWorkshopService.shared
    @State private var activationCode = ""
    @State private var accepted = false
    @State private var confirmingRemoval = false
    @State private var copiedDevice = false
    @State private var copiedGroup = false

    var body: some View {
        Section {
            VStack(alignment: .leading, spacing: 12) {
                Toggle("免登录下载", isOn: Binding(get: { service.isEnabled }, set: { service.setEnabled($0) }))
                    .disabled(!service.isActivated || service.isChecking || !service.isAvailable)
                Text("无需登录 Steam 或拥有 Wallpaper Engine，即可下载公开壁纸。此模式仅支持下载，不支持 Steam 订阅、收藏或评论。")
                    .font(.caption).foregroundStyle(.secondary)
                Text("为控制资源使用，本功能限量开放。QQ群 2160040437 不定期发放激活资格，领取时请提供下方设备码。激活码一机一码，仅适用于本机。")
                    .font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button(LocalizedStringKey(copiedGroup ? "已复制群号" : "复制QQ群号")) {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString("2160040437", forType: .string)
                        copiedGroup = true
                    }
                    if !service.deviceCode.isEmpty {
                        Button(LocalizedStringKey(copiedDevice ? "已复制设备码" : "复制设备码")) {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(service.deviceCode, forType: .string)
                            copiedDevice = true
                        }
                    }
                    Spacer()
                    if service.isChecking { ProgressView().controlSize(.small) }
                }
                if !service.deviceCode.isEmpty {
                    Text(service.deviceCode).font(.system(.caption2, design: .monospaced)).textSelection(.enabled)
                }
                if service.isActivated {
                    HStack {
                        Label("本机已激活", systemImage: "checkmark.seal.fill").foregroundStyle(.green)
                        Spacer()
                        Button("移除激活", role: .destructive) { confirmingRemoval = true }
                    }
                    if let date = service.expiresAt {
                        Text(L("有效期至 %@", date.formatted(date: .numeric, time: .omitted))).font(.caption)
                    }
                } else if service.isAvailable {
                    SecureField("输入本机激活码", text: $activationCode)
                        .textFieldStyle(.roundedBorder)
                    Text("免登录下载使用第三方服务，作品标识和网络信息会发送给相关服务。服务可能失效，激活不代表获得 Steam 许可证，也不保证符合平台条款。")
                        .font(.caption).foregroundStyle(.secondary)
                    Toggle("我已了解并接受上述限制与风险", isOn: $accepted).font(.caption)
                    Button("激活并开启") { service.activate(activationCode); activationCode = "" }
                        .buttonStyle(.borderedProminent)
                        .disabled(!accepted || activationCode.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || service.isChecking)
                }
                if let message = service.errorMessage {
                    Text(message).font(.caption).foregroundStyle(.red)
                }
                Text("更改立即生效；关闭此模式会取消正在进行的免登录下载。")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        } header: {
            Label("免登录工坊下载", systemImage: "arrow.down.circle.badge.checkmark")
        }
        .task { service.refresh() }
        .confirmationDialog("移除本机激活？", isPresented: $confirmingRemoval) {
            Button("移除激活", role: .destructive) { service.removeActivation() }
        } message: {
            Text("将关闭免登录下载并清除本机保存的激活码，已下载壁纸不受影响。")
        }
    }
}
