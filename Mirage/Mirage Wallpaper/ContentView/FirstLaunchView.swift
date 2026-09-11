//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct FirstLaunchView: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(GlobalSettingsViewModel.self) private var globalSettingsViewModel
    
    @State var checked = false
    
    var body: some View {
        @Bindable var globalSettingsViewModel = globalSettingsViewModel
        VStack {
            VStack(spacing: 5) {
                Text("欢迎使用 Mirage")
                    .font(.largeTitle)
                Divider()
            }
            .fixedSize()
            VStack {
                Group {
                    NewSection(title: "三类壁纸，一站渲染",
                               text: "支持 Wallpaper Engine 的场景、网页、视频三类壁纸，由专用引擎以独立进程渲染到桌面，兼顾画质与稳定。",
                               systemImage: "square.stack.3d.up",
                               imageColor: .purple)
                    NewSection(title: "自动加载创意工坊壁纸",
                               text: "自动读取 Steam 创意工坊已订阅的壁纸，也可将本地文件夹或视频导入到 Mirage 自有壁纸库。",
                               systemImage: "square.and.arrow.down")
                    NewSection(title: "熟悉的界面布局",
                               text: "沿用 Wallpaper Engine 的界面布局，上手无门槛，并针对 macOS 做了本地化与视觉优化。",
                               systemImage: "macwindow.on.rectangle",
                               imageColor: .yellow)
                    NewSection(title: "实时属性调节",
                               text: "根据壁纸自带的属性动态生成调节控件，音量、速度、颜色、开关等即改即生效。",
                               systemImage: "slider.horizontal.3",
                               imageColor: .green)
                }
                .fixedSize(horizontal: false, vertical: true)
                .padding(.vertical)
                .padding(.horizontal, 50)
            }
            ProjectFeedbackBanner(showsActions: false)
                .padding(.horizontal, 20)
                .padding(.bottom, 8)
            Button {
                UserDefaults.standard.set(!checked, forKey: "IsFirstLaunch")
                dismiss()
            } label: {
                Text("开始使用")
                    .frame(width: 100)
            }
            .buttonStyle(.borderedProminent)
            HStack {
                Toggle("在下次更新前不再显示", isOn: $checked)
                Spacer()
            }
        }
        .textSelection(.enabled)
        .padding()
        .frame(width: 600)
    }
}

extension FirstLaunchView {
    struct NewSection: View {
        var title: LocalizedStringKey
        var text: LocalizedStringKey
        var textColor: Color = .primary
        var systemImage: String
        var imageColor: Color = .accentColor
        
        var body: some View {
            HStack {
                Image(systemName: systemImage)
                    .frame(width: 50, height: 50)
                    .font(.largeTitle)
                    .foregroundStyle(imageColor)
                VStack(alignment: .leading) {
                    Text(title)
                        .foregroundStyle(textColor)
                        .font(.title2)
                        .bold()
                    Text(text)
                        .foregroundStyle(textColor)
                }
                .multilineTextAlignment(.leading)
                Spacer()
            }
        }
    }
}

extension AppDelegate {
    @objc func resetFirstLaunch() {
        UserDefaults.standard.set(true, forKey: "IsFirstLaunch")
    }
}

struct FirstLaunchView_Previews: PreviewProvider {
    static var previews: some View {
        FirstLaunchView()
    }
}
