//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import SwiftUI

/// Narrows byte-level download invalidations to the card status itself.  The
/// surrounding grid and its native context-menu host remain stable.
struct WorkshopItemDownloadStatus<Content: View>: View {
    let workshopID: String
    @ObservedObject private var downloadStore: WorkshopDownloadStore
    private let content: (DownloadState?) -> Content

    init(
        workshopID: String,
        downloadStore: WorkshopDownloadStore,
        @ViewBuilder content: @escaping (DownloadState?) -> Content
    ) {
        self.workshopID = workshopID
        self.downloadStore = downloadStore
        self.content = content
    }

    var body: some View {
        content(downloadStore.state(for: workshopID))
    }
}

struct WorkshopActiveDownloadCount<Content: View>: View {
    @ObservedObject private var downloadStore: WorkshopDownloadStore
    private let content: (Int) -> Content

    init(
        downloadStore: WorkshopDownloadStore,
        @ViewBuilder content: @escaping (Int) -> Content
    ) {
        self.downloadStore = downloadStore
        self.content = content
    }

    var body: some View {
        content(downloadStore.activeCount)
    }
}
