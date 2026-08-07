//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct UnsafeWallpaper: View {
    @Environment(\.dismiss) var dismiss

    let request: ContentViewModel.PendingTrustRequest

    private var wallpaper: WEWallpaper { request.wallpaper }

    @State var seconds: Int = 5
    @State var isIgnored = false

    var typeStringDict: [String : String] =
    [
        "web": "网页",
        "application": "应用程序"
    ]

    init(request: ContentViewModel.PendingTrustRequest) {
        self.request = request
    }

    var body: some View {
        VStack(spacing: 0) {
            Text(L("正在打开%@类壁纸", L(typeStringDict[wallpaper.project.type.lowercased()] ?? "未知")))
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
                .font(.title2)
            Divider()
            HStack(spacing: 20) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .symbolRenderingMode(.palette)
                    .foregroundStyle(.white, .red)
                    .shadow(radius: 6)
                    .frame(maxWidth: 100)
                VStack(alignment: .leading, spacing: 10) {
                    Text(L("你即将把以下%@类文件作为壁纸运行：", L(typeStringDict[wallpaper.project.type.lowercased()] ?? "未知来源")))
                    Text(wallpaper.resolvedEntryURL.path(percentEncoded: false)).bold()
                    Text("Mirage 无法控制该文件的行为，网页壁纸可能包含可执行脚本。请确认它来自可信来源后再继续。")
                    Text(seconds > 0 ? "请等待 \(seconds) 秒。" : "请注意潜在的恶意代码风险。")
                    Toggle("对此壁纸不再提示", isOn: $isIgnored)
                }
                .frame(maxWidth: .infinity)
            }
            .frame(maxHeight: .infinity)
            .padding(.horizontal)
            Divider()
            HStack {
                Button {
                    let vm = AppDelegate.shared.wallpaperViewModel

                    // Trust MUST be recorded before the wallpaper is applied.
                    // Applying assigns `currentWallpaper`, whose `didSet` walks
                    // straight through to `RendererController.render`, which
                    // vetoes any web wallpaper that is not trusted *at that
                    // moment*. Recording consent afterwards meant the launch the
                    // user just authorized was silently blocked by Mirage's own
                    // backstop — the wallpaper appeared to do nothing on click.
                    if isIgnored {
                        vm.trust(wallpaper)
                    } else {
                        WallpaperViewModel.trustForSession(wallpaper)
                    }

                    switch request.action {
                    case .applyToCurrent:
                        vm.currentWallpaper = wallpaper
                    case .applyOnDisplay(let displayID):
                        vm.applyOnDisplay(wallpaper, displayID: displayID)
                    case .applyToAllDisplays:
                        vm.applyToAllDisplays(wallpaper)
                    }

                    dismiss()
                } label: {
                    Text("继续")
                        .padding(.horizontal, 10)
                }
                .animation(.default, value: seconds)
                .buttonStyle(.borderedProminent)
                .tint(.red)
                .disabled(seconds > 0 ? true : false)
                Button {
                    dismiss()
                } label: {
                    Text("取消")
                        .padding(.horizontal, 10)
                }
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .trailing)
        }
        .onAppear {
            let _ = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { timer in
                if self.seconds <= 0 {
                    timer.invalidate()
                } else {
                    self.seconds -= 1
                }
            }
        }
    }
}
