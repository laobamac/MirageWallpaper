//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct PlaylistOpenSheet: View {
    @ObservedObject var manager: PlaylistManager
    let screen: Int
    @Binding var isPresented: Bool

    @State private var pendingDelete: Playlist?

    private var dateFormatter: DateFormatter {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        return f
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(L("打开播放列表"))
                .font(.title2.bold())

            if manager.saved.isEmpty {
                VStack {
                    Spacer()
                    Text(L("您尚未创建任何播放列表。"))
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .center)
                    Spacer()
                }
                .frame(height: 180)
            } else {
                ScrollView {
                    VStack(spacing: 6) {
                        ForEach(manager.saved) { playlist in
                            row(playlist)
                        }
                    }
                }
                .frame(height: 260)
            }

            HStack {
                Spacer()
                Button(L("完成")) { isPresented = false }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 460)
        .confirmationDialog(
            L("删除"),
            isPresented: Binding(
                get: { pendingDelete != nil },
                set: { if !$0 { pendingDelete = nil } }
            ),
            presenting: pendingDelete
        ) { playlist in
            Button(L("删除"), role: .destructive) {
                manager.deleteSaved(playlist.id)
                pendingDelete = nil
            }
            Button(L("取消"), role: .cancel) { pendingDelete = nil }
        } message: { playlist in
            Text("“\(playlist.name)”")
        }
    }

    private func row(_ playlist: Playlist) -> some View {
        HStack(spacing: 12) {
            Image(systemName: "list.and.film")
                .font(.title3)
                .foregroundStyle(.tint)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(playlist.name)
                    .font(.headline)
                    .lineLimit(1)
                Text("\(playlist.items.count) · \(dateFormatter.string(from: playlist.updatedAt))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button(L("读取")) {
                manager.load(saved: playlist, into: screen)
                isPresented = false
            }
            .buttonStyle(.borderedProminent)
            Button {
                pendingDelete = playlist
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.red)
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 10).fill(.quaternary.opacity(0.4)))
    }
}
