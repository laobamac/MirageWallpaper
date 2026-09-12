//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

import AppKit
import ExtensionFoundation
import Foundation
import QuartzCore

struct MirageWallpaperExtensionConfiguration: AppExtensionConfiguration {
    func accept(connection: NSXPCConnection) -> Bool {
        let exported = NSXPCInterface(with: WallpaperExtensionXPCProtocol.self)
        let names = [
            "WallpaperIDXPC", "WallpaperCreationRequestXPC", "WallpaperUpdateRequestXPC",
            "WallpaperRemoteContextXPC", "WallpaperSnapshotXPC", "WallpaperContentTypeSetXPC",
            "WallpaperChoiceIDXPC", "WallpaperChoiceIDsXPC", "WallpaperExtensionChoiceRequestXPC",
            "WallpaperChoiceRequestAdditionResultXPC", "WallpaperDebugRequestXPC", "WallpaperDebugResponseXPC",
            "WallpaperMigrationVersionXPC", "WallpaperSettingsViewModelsXPC", "AuditTokenXPC"
        ]
        let classes = NSMutableSet()
        names.compactMap { objc_getClass($0) }.forEach { classes.add($0) }
        [NSString.self, NSNumber.self, NSData.self, NSArray.self, NSDictionary.self, NSURL.self, NSError.self]
            .forEach { classes.add($0) }
        let allowed = classes as! Set<AnyHashable>
        let selectors: [(Selector, Int, Bool)] = [
            (#selector(MirageWallpaperXPCHandler.acquire(withId:request:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.acquire(withId:request:reply:)), 1, false),
            (#selector(MirageWallpaperXPCHandler.acquire(withId:request:reply:)), 0, true),
            (#selector(MirageWallpaperXPCHandler.update(withId:request:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.update(withId:request:reply:)), 1, false),
            (#selector(MirageWallpaperXPCHandler.invalidate(withId:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.snapshot(withId:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.snapshot(withId:reply:)), 0, true),
            (#selector(MirageWallpaperXPCHandler.provideSettingsViewModels(withContentTypes:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.provideSettingsViewModels(withContentTypes:reply:)), 0, true),
            (#selector(MirageWallpaperXPCHandler.addChoiceRequest(withChoiceRequest:onBehalfOfProcess:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.addChoiceRequest(withChoiceRequest:onBehalfOfProcess:reply:)), 1, false),
            (#selector(MirageWallpaperXPCHandler.removeChoiceRequest(withChoiceRequest:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.selectedChoicesDidChange(for:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.invokeContextMenuAction(withMenuItemID:groupItemID:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.invokeContextMenuAction(withMenuItemID:groupItemID:reply:)), 1, false),
            (#selector(MirageWallpaperXPCHandler.isChoiceDownloaded(with:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.download(withChoiceID:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.pauseDownload(for:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.cancelDownload(for:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.resumeDownload(for:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.removeDownload(for:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.handleDebugRequest(for:reply:)), 0, false),
            (#selector(MirageWallpaperXPCHandler.handleDebugRequest(for:reply:)), 0, true),
            (#selector(MirageWallpaperXPCHandler.handleNotification(withNamed:reply:)), 0, false)
        ]
        selectors.forEach { exported.setClasses(allowed, for: $0.0, argumentIndex: $0.1, ofReply: $0.2) }
        connection.exportedInterface = exported
        connection.remoteObjectInterface = NSXPCInterface(with: WallpaperExtensionProxyXPCProtocol.self)
        let handler = MirageWallpaperXPCHandler()
        connection.exportedObject = handler
        handler.agentProxy = connection.remoteObjectProxy as? WallpaperExtensionProxyXPCProtocol
        connection.invalidationHandler = { [weak handler] in handler?.invalidateAll() }
        connection.resume()
        return true
    }
}

@main
final class MirageWallpaperExtension: NSObject, AppExtension {
    typealias Configuration = MirageWallpaperExtensionConfiguration

    var configuration: MirageWallpaperExtensionConfiguration { MirageWallpaperExtensionConfiguration() }

    override required init() {
        super.init()
        if #available(macOS 26.0, *) {
            _ = dlopen("/System/Library/PrivateFrameworks/WallpaperExtensionKit.framework/WallpaperExtensionKit", RTLD_LAZY)
        }
    }
}
