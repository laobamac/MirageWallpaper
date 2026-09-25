#pragma once

#import <Foundation/Foundation.h>
#import <WebKit/WebKit.h>

NS_ASSUME_NONNULL_BEGIN

// Serves the wallpaper directory over a custom URL scheme (we-wallpaper://)
// so XHR / fetch / media work same-origin.
//
// Why: OWE's CEF build uses --allow-file-access-from-files + --disable-web-security
// to let file:// pages XHR sibling files (Spine loads .skel/.atlas via
// XMLHttpRequest). WKWebView has no equivalent — XHR/fetch to file:// is
// blocked by the file-origin sandbox. Serving the workshop dir through a
// custom scheme gives the page a normal same-origin base, so everything
// works as on a web server.
//
// The engine loads the page as we-wallpaper://wallpaper/<entryHTML>; every
// relative resource resolves to we-wallpaper://wallpaper/... and is served here.
@interface WRURLSchemeHandler : NSObject <WKURLSchemeHandler>

// Set before the first request (before -[WKWebView loadRequest:]). The handler
// is registered on the WKWebViewConfiguration at init time, before the workshop
// directory is known, so this is updated in openWallpaper.
@property (nonatomic, copy) NSString *baseDirectory;
@property (nonatomic, copy) NSArray<NSString *> *overlayDirectories;
@property (nonatomic, assign) BOOL loadFromMemory;

// Drops all bytes retained by memory mode. Called before opening a new wallpaper.
- (void)clearMemoryCache;

- (instancetype)init;
- (instancetype)initWithBaseDirectory:(NSString *)baseDirectory;

@end

NS_ASSUME_NONNULL_END
