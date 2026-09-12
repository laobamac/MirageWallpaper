//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import Foundation

private func mirageLocalized(_ key: String) -> String {
    Bundle.main.localizedString(forKey: key, value: key, table: "Localizable")
}

func buildMirageSettingsViewModels(configuration: MirageLockConfiguration) throws -> AnyObject {
    let provider = MirageChoiceProviderID(Bundle.main.bundleIdentifier ?? "cn.laobamac.Mirage.WallpaperExtension")
    let fallbackThumbnail = URL(fileURLWithPath: "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/SidebarDisplay.icns")
    let thumbnailURL = Bundle.main.url(forResource: "thumbnail", withExtension: "png") ?? fallbackThumbnail
    let items = configuration.displays.values.sorted { $0.displayID < $1.displayID }.map { display in
        let identifier = "display-\(display.displayID)"
        let choiceID = MirageChoiceID(
            id: identifier,
            descriptor: MirageChoiceIDDescriptor(
                provider: provider,
                identifier: identifier,
                files: [],
                configuration: Data(identifier.utf8)
            )
        )
        let thumbnail = MirageThumbnail.image(thumbnailURL)
        return MirageSettingsItem(
            id: choiceID,
            localizedName: "Mirage · \(display.title)",
            thumbnail: thumbnail,
            choice: MirageChoiceDescriptor(
                id: choiceID,
                provider: provider,
                identifier: identifier,
                name: display.title,
                localizedDescription: mirageLocalized("Mirage 动态锁屏显示器实例"),
                thumbnail: thumbnail,
                isDownloaded: true,
                options: []
            ),
            contentBadge: .dynamic,
            showInTopLevel: true,
            sortOrder: Int(display.displayID),
            disposability: .none
        )
    }
    let group = MirageSettingsGroup(
        id: MirageGroupID(id: "mirage-dynamic-lock-screen"),
        items: items,
        localizedName: mirageLocalized("Mirage 动态锁屏"),
        disposability: .none,
        sortOrder: -100,
        sortID: MirageGroupSortID(id: "com.apple.wallpaper.aerials"),
        allChoiceID: nil,
        shouldHideItemLabels: false,
        contextMenu: nil,
        thumbnail: nil
    )
    let models = MirageSettingsViewModels(
        desktop: MirageSettingsViewModel(groups: configuration.enabled == false ? [] : [group],
                                        refreshPolicy: .default, isModificationDisabled: false),
        screenSaver: nil
    )
    guard let encoded = mirageSettingsViewModelsXPC(models) else {
        throw MirageLockBridge.failure("Unable to encode wallpaper settings")
    }
    return encoded
}
