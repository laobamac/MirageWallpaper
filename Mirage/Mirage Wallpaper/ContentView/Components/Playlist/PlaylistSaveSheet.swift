//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

struct PlaylistSaveSheet: View {
    @ObservedObject var manager: PlaylistManager
    let screen: Int
    @Binding var isPresented: Bool

    @State private var name: String = ""
    @State private var didPrefill = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(L("保存播放列表"))
                .font(.title2.bold())

            VStack(alignment: .leading, spacing: 6) {
                Text(L("名称"))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                TextField("", text: $name)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(save)
            }

            Text(L("如果已存在相同名称的播放列表，则它将会被覆盖。"))
                .font(.footnote)
                .foregroundStyle(.secondary)

            Spacer(minLength: 4)

            HStack {
                Spacer()
                Button(L("取消")) { isPresented = false }
                Button(action: save) {
                    Text(L("保存")).frame(width: 60)
                }
                .buttonStyle(.borderedProminent)
                .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 380)
        .onAppear {
            guard !didPrefill else { return }
            didPrefill = true
            let current = manager.current(on: screen)
            name = current.name
        }
    }

    private func save() {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return }
        _ = manager.saveAs(name: trimmed, from: screen)
        isPresented = false
    }
}
