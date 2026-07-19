//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

extension AppDelegate {
    @objc func showAboutUs() {
        // "About" now lives inside the unified settings panel that floats over
        // the main window. Open the panel and select the About section.
        globalSettingsViewModel.selection = 4
        openSettingsWindow()
    }
}

struct AboutUsView: View {
    @ObservedObject private var localization = MirageLocalization.shared
    @State private var copiedUSDTAddress = false

    private let afdianURL = URL(string: "https://www.ifdian.net/a/laobamac")!
    private let usdtAddress = "0xFc0a5C52e3A085FEc7b077FE3D2C413114Bf880D"
    private var version: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "1.0.0"
    }

    private var build: String {
        Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "开发构建"
    }

    private var commit: String {
        String((Bundle.main.infoDictionary?["MirageGitCommit"] as? String ?? "unknown").prefix(12))
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 24) {
                HStack(spacing: 20) {
                    if let icon = NSImage(named: "AppIcon") {
                        Image(nsImage: icon)
                            .resizable()
                            .frame(width: 88, height: 88)
                    }
                    Divider().frame(maxHeight: 90)
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Mirage").bold().font(.largeTitle)
                        Text("macOS 动态壁纸引擎").font(.footnote).foregroundStyle(.secondary)
                        Text("场景 · 网页 · 视频").font(.caption).foregroundStyle(.tertiary)
                    }
                }

                VStack(spacing: 14) {
                    Text("版本 \(version)（构建 \(build)）").foregroundStyle(.secondary)
                    Text("提交 \(commit)").font(.caption.monospaced()).foregroundStyle(.tertiary)
                    HStack(spacing: 4) {
                        Text("作者")
                        Text("王孝慈 (laobamac)").bold()
                    }
                    Link("github.com/laobamac/MirageWallpaper",
                         destination: URL(string: "https://github.com/laobamac/MirageWallpaper")!)
                        .font(.footnote)
                }
                .font(.callout)

                sponsorSection

                ProjectFeedbackBanner(showsActions: false)
            }
            .frame(maxWidth: .infinity)
            .padding(.horizontal, 28)
            .padding(.vertical, 24)
        }
        .textSelection(.enabled)
        .environment(\.locale, localization.locale)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var sponsorSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Image(systemName: "heart.fill")
                    .foregroundStyle(.pink)
                Text("支持 Mirage")
                    .font(.title3.bold())
            }

            Text("Mirage 会继续免费开放开发。若它为你的桌面带来了价值，欢迎按自己的意愿赞助；每一份支持都会用于持续维护与兼容性改进。")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)

            // A flexible grid keeps every donation option visible regardless of
            // the container width instead of being clipped by a fixed-width HStack.
            LazyVGrid(
                columns: [GridItem(.adaptive(minimum: 150), spacing: 16, alignment: .top)],
                alignment: .leading,
                spacing: 16
            ) {
                Link(destination: afdianURL) {
                    SponsorQRCode(resource: "afdian", fileExtension: "jpg", title: "爱发电", subtitle: "点击打开爱发电")
                }
                .buttonStyle(.plain)

                SponsorQRCode(resource: "wechat-pay", fileExtension: "png", title: "微信支付", subtitle: "使用微信扫一扫")
                SponsorQRCode(resource: "alipay", fileExtension: "jpg", title: "支付宝", subtitle: "使用支付宝扫一扫")

                usdtCard
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.pink.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
        .overlay {
            RoundedRectangle(cornerRadius: 12)
                .stroke(Color.pink.opacity(0.25))
        }
    }

    private var usdtCard: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("USDT", systemImage: "bitcoinsign.circle")
                .font(.headline)
            Text("海外赞助")
                .font(.caption)
                .foregroundStyle(.secondary)
            Text("接收 USDT")
                .font(.caption)
            Text(usdtAddress)
                .font(.caption.monospaced())
                .textSelection(.enabled)
                .lineLimit(3)
                .fixedSize(horizontal: false, vertical: true)
            Button {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(usdtAddress, forType: .string)
                copiedUSDTAddress = true
            } label: {
                Label(copiedUSDTAddress ? "地址已复制" : "复制地址", systemImage: copiedUSDTAddress ? "checkmark" : "doc.on.doc")
            }
            .buttonStyle(.bordered)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private struct SponsorQRCode: View {
    let resource: String
    let fileExtension: String
    let title: LocalizedStringKey
    let subtitle: LocalizedStringKey

    var body: some View {
        VStack(spacing: 6) {
            if let url = Bundle.main.url(forResource: resource, withExtension: fileExtension),
               let image = NSImage(contentsOf: url) {
                Image(nsImage: image)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
                    .frame(width: 118, height: 154)
                    .clipShape(RoundedRectangle(cornerRadius: 8))
            }
            Text(title).font(.headline)
            Text(subtitle)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .frame(width: 118)
    }
}

struct AboutUsView_Previews: PreviewProvider {
    static var previews: some View {
        AboutUsView()
    }
}
