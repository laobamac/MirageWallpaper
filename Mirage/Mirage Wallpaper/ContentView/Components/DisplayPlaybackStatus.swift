import SwiftUI

struct DisplayPlaybackSummary: Identifiable {
    let id: DisplayKey
    let displayName: String
    let wallpaperTitle: String
    let status: String

    static func resolve(id: DisplayKey, name: String, activeTitle: String?, assignedTitle: String?,
                        pending: Bool, running: Bool, policy: GSPlayback,
                        sessionPaused: Bool, lockPaused: Bool, muted: Bool = false, runtime: WallpaperRuntimeState) -> Self {
        let title = activeTitle ?? assignedTitle.map { L("已配置：%@", $0) } ?? L("未设置壁纸")
        let status: String
        if pending { status = L("正在切换…") }
        else if assignedTitle == nil && activeTitle == nil { status = L("未设置") }
        else if lockPaused { status = L("锁屏暂停") }
        else if policy == .stop { status = L("按规则停止") }
        else if !running { status = L("未运行") }
        else if sessionPaused || runtime.speed == 0 { status = L("已暂停") }
        else if policy == .pause { status = L("按规则暂停") }
        else if muted || policy == .mute || runtime.muted || runtime.volume == 0 { status = L("播放中（静音）") }
        else { status = L("播放中") }
        return Self(id: id, displayName: name, wallpaperTitle: title, status: status)
    }
}

struct DisplayPlaybackBar: View {
    let summaries: [DisplayPlaybackSummary]
    let selected: DisplayKey
    var select: (DisplayKey) -> Void

    var body: some View {
        ScrollView(.horizontal) {
            HStack(spacing: 8) {
                ForEach(summaries) { summary in
                    row(summary)
                }
            }
        }
        .scrollIndicators(.hidden)
        .frame(height: 58)
    }
    private func row(_ summary: DisplayPlaybackSummary) -> some View {
        let background: Color = selected == summary.id ? .accentColor.opacity(0.12) : .secondary.opacity(0.06)
        let label = summary.displayName + " · " + summary.status
        return Button { select(summary.id) } label: {
                        VStack(alignment: .leading, spacing: 3) {
                            Text(label).font(.caption.bold())
                            Text(summary.wallpaperTitle).font(.caption).foregroundStyle(.secondary)
                        }
                        .lineLimit(1)
                        .frame(width: 210, alignment: .leading)
                        .padding(8)
                        .background(background,
                                    in: RoundedRectangle(cornerRadius: 8))
                    }
                    .buttonStyle(.plain)
                    .help(label + "\n" + summary.wallpaperTitle)
                    .accessibilityLabel(label + " · " + summary.wallpaperTitle)
    }

}

struct DisplayPlaybackStatusView: View {
    @Bindable var wallpaperViewModel: WallpaperViewModel
    let isActive: Bool

    var body: some View {
        TimelineView(.animation(minimumInterval: 1, paused: !isActive)) { _ in
            DisplayPlaybackBar(
                summaries: wallpaperViewModel.connectedDisplays.map { wallpaperViewModel.playbackSummary(for: $0) },
                selected: wallpaperViewModel.selectedDisplayKey,
                select: { wallpaperViewModel.selectedDisplayKey = $0 })
        }
    }
}
