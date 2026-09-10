//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#import "VideoRendererEngine.h"
#import "VideoManifest.h"
#import <AppKit/AppKit.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>

static void Check(bool value, const char* message) {
    if (value) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static bool Wait(BOOL (^condition)(void), double seconds) {
    NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!condition() && [deadline timeIntervalSinceNow] > 0) {
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    return condition();
}

static std::array<double, 3> Sample(NSBitmapImageRep* image, double x, double y) {
    NSInteger px = MIN(image.pixelsWide - 1, MAX(0, (NSInteger)floor(x)));
    NSInteger py = MIN(image.pixelsHigh - 1, MAX(0, (NSInteger)floor(y)));
    NSColor* color = [[image colorAtX:px y:py] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    return { color.redComponent, color.greenComponent, color.blueComponent };
}

static NSBitmapImageRep* Snapshot(VRVideoRendererEngine* engine, NSString* path) {
    __block BOOL complete = NO;
    __block BOOL success = NO;
    [engine takeSnapshotToPath:path completion:^(BOOL ok) { success = ok; complete = YES; }];
    Check(Wait(^BOOL { return complete; }, 8), "snapshot completes");
    Check(success, "snapshot succeeds while playback is paused");
    NSBitmapImageRep* image = [[NSBitmapImageRep alloc] initWithData:[NSData dataWithContentsOfFile:path]];
    Check(image != nil, "snapshot image decodes");
    return image;
}

int main(int argc, const char** argv) {
    @autoreleasepool {
        Check(argc == 2, "fixture directory argument exists");
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        NSString* root = [NSString stringWithUTF8String:argv[1]];
        for (int test = 0; test < 3; ++test) {
            @autoreleasepool {
                const bool rotated = test == 2;
                NSString* directory = [root stringByAppendingPathComponent:rotated ? @"rotated" : @"base"];
                NSError* error = nil;
                VRVideoManifest* manifest = [VRVideoManifest loadFromDirectory:directory error:&error];
                Check(manifest != nil, "generated manifest loads");
                NSRect bounds = rotated ? NSMakeRect(0, 0, 192, 108) : NSMakeRect(0, 0, 108, 192);
                VRVideoEngineConfig config = [VRVideoRendererEngine defaultConfig];
                config.loadFromMemory = test == 1;
                config.muted = YES;
                config.positionX = 0;
                config.positionY = 0;
                VRVideoRendererEngine* engine = [[VRVideoRendererEngine alloc] initWithFrame:bounds config:config];
                NSWindow* window = [[NSWindow alloc] initWithContentRect:bounds styleMask:NSWindowStyleMaskBorderless
                                                                backing:NSBackingStoreBuffered defer:NO];
                window.releasedWhenClosed = NO;
                window.alphaValue = 0;
                window.contentView = engine;
                [window orderBack:nil];
                __block BOOL ready = NO;
                __block BOOL failed = NO;
                __block BOOL canX = NO, canY = NO;
                __block BOOL gotAvailability = NO;
                engine.firstFrameReadyBlock = ^{ ready = YES; };
                engine.videoDidFailBlock = ^(NSString* message) { failed = YES; fprintf(stderr, "%s\n", message.UTF8String); };
                engine.positionAvailabilityBlock = ^(BOOL x, BOOL y) { canX = x; canY = y; gotAvailability = YES; };
                Check([engine openWallpaper:manifest error:&error], "generated video opens");
                Check(Wait(^BOOL { return ready || failed; }, 12) && ready && !failed, "video decodes its first frame");
                Check(Wait(^BOOL { return gotAvailability; }, 2), "layout availability arrives");
                AVAssetTrack* track = [engine.player.currentItem.asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
                CGAffineTransform transform = track.preferredTransform;
                AVPlayerLayer* playerLayer = (AVPlayerLayer*)engine.layer.sublayers.firstObject;
                fprintf(stdout, "natural=%.0fx%.0f transform=%.1f,%.1f,%.1f,%.1f rect=%.1fx%.1f\n", track.naturalSize.width,
                        track.naturalSize.height, transform.a, transform.b, transform.c, transform.d,
                        playerLayer.videoRect.size.width, playerLayer.videoRect.size.height);
                fprintf(stdout, "case=%d presentation=%.0fx%.0f axes=%d,%d\n", test,
                        engine.player.currentItem.presentationSize.width,
                        engine.player.currentItem.presentationSize.height, canX, canY);
                Check(rotated ? (!canX && canY) : (canX && !canY), "availability follows the oriented source");
                [engine pause];
                AVAssetImageGenerator* generator = [[AVAssetImageGenerator alloc] initWithAsset:engine.player.currentItem.asset];
                generator.appliesPreferredTrackTransform = YES;
                CGImageRef cgReference = [generator copyCGImageAtTime:kCMTimeZero actualTime:NULL error:&error];
                Check(cgReference != NULL, "oriented reference frame decodes");
                NSBitmapImageRep* reference = [[NSBitmapImageRep alloc] initWithCGImage:cgReference];
                CGImageRelease(cgReference);
                const double sw = reference.pixelsWide, sh = reference.pixelsHigh;
                for (double position : { 0.0, 0.5, 1.0 }) {
                    const double x = rotated ? 0.5 : position;
                    const double y = rotated ? position : 0.5;
                    [engine setPositionX:x y:y];
                    [engine layout];
                    NSString* path = [root stringByAppendingPathComponent:[NSString stringWithFormat:@"case-%d-%.1f.heic", test, position]];
                    NSBitmapImageRep* snapshot = Snapshot(engine, path);
                    const double scale = fmax(bounds.size.width / sw, bounds.size.height / sh);
                    const double cw = bounds.size.width / scale, ch = bounds.size.height / scale;
                    const auto expected = Sample(reference, x * (sw - cw) + cw / 2, y * (sh - ch) + ch / 2);
                    const auto actual = Sample(snapshot, snapshot.pixelsWide / 2.0, snapshot.pixelsHigh / 2.0);
                    fprintf(stdout, "position=%.1f expected=%.2f,%.2f,%.2f actual=%.2f,%.2f,%.2f\n", position,
                            expected[0], expected[1], expected[2], actual[0], actual[1], actual[2]);
                    int expectedChannel = 0, actualChannel = 0;
                    for (int c = 1; c < 3; ++c) {
                        if (expected[c] > expected[expectedChannel]) expectedChannel = c;
                        if (actual[c] > actual[actualChannel]) actualChannel = c;
                    }
                    Check(expectedChannel == actualChannel && actual[actualChannel] > 0.6,
                          "snapshot crop matches AVFoundation's oriented reference");
                    Check(engine.player.rate == 0, "position updates keep playback paused");
                    Check(fabs((double)snapshot.pixelsWide / snapshot.pixelsHigh - bounds.size.width / bounds.size.height) < 0.001,
                          "snapshot uses the display aspect ratio");
                }
                [engine setFillMode:VRVideoFillModeContain];
                NSBitmapImageRep* fitted = Snapshot(engine, [root stringByAppendingPathComponent:[NSString stringWithFormat:@"contain-%d.heic", test]]);
                auto corner = Sample(fitted, 1, 1);
                Check(corner[0] < 0.08 && corner[1] < 0.08 && corner[2] < 0.08, "contain snapshots retain the letterbox background");
                [window close];
            }
        }
        puts("VideoPositionIntegration: ok");
    }
    return 0;
}
