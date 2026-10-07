// Copyright © 2026 王孝慈. All rights reserved.

#import "WRLive2DCompatibility.h"
#import <CommonCrypto/CommonDigest.h>

#include "WRLive2DCompatibilityScript.h"

static NSString *WRScriptDigest(NSData *data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *result = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (NSUInteger i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        [result appendFormat:@"%02x", digest[i]];
    }
    return result;
}

NSString *WRLive2DScriptETag(NSData *source) {
    return [NSString stringWithFormat:@"\"wr-live2d-%@\"", WRScriptDigest(source)];
}

NSData *WRAdaptLive2DScript(NSData *source) {
    if (source.length == 0 || source.length > 4 * 1024 * 1024) return nil;
    NSString *script = [[NSString alloc] initWithData:source encoding:NSUTF8StringEncoding];
    if (script == nil || [script containsString:@"__wrLive2DCompatibility"]) return nil;

    NSString *boundary = @"  const WL = window.WallpaperLayout;";
    NSArray<NSString *> *parts = [script componentsSeparatedByString:boundary];
    if (parts.count != 2) return nil;
    NSData *runtime = [parts[0] dataUsingEncoding:NSUTF8StringEncoding];
    if (![WRScriptDigest(runtime) isEqualToString:
          @"07d88ba7497b331d76781cc1d72e056a2a202b48a9027ffa24245b1dbe76fdc8"]) return nil;

    NSString *application = @"    app = new Application({\n"
        "      view: document.getElementById(\"canvas\"),\n"
        "      transparent: true,\n"
        "      autoStart: true,\n"
        "      resizeTo: window,\n"
        "      antialias: true\n"
        "    });";
    if ([script componentsSeparatedByString:application].count != 2) return nil;

    NSMutableString *adapted = [parts[0] mutableCopy];
    for (NSString *upload in @[
        @"this.gl.bufferData(this.gl.ARRAY_BUFFER, vertexArray, this.gl.DYNAMIC_DRAW);",
        @"this.gl.bufferData(this.gl.ARRAY_BUFFER, uvArray, this.gl.DYNAMIC_DRAW);",
        @"this.gl.bufferData(this.gl.ELEMENT_ARRAY_BUFFER, indexArray, this.gl.DYNAMIC_DRAW);"
    ]) {
        [adapted replaceOccurrencesOfString:upload
                                withString:[@"if (!bufferData.__wrUploaded) " stringByAppendingString:upload]
                                   options:0 range:NSMakeRange(0, adapted.length)];
    }
    [adapted appendString:[NSString stringWithUTF8String:kWRLive2DCompatibilityJS]];
    [adapted appendString:@"\n"];
    [adapted appendString:boundary];
    [adapted appendString:parts[1]];

    NSString *retinaApplication = [application stringByReplacingOccurrencesOfString:
        @"      resizeTo: window,\n" withString:
        @"      resizeTo: window,\n"
         "      resolution: window.devicePixelRatio || 1,\n"
         "      autoDensity: true,\n"];
    retinaApplication = [retinaApplication stringByAppendingString:
        @"\n    __wrLive2DCompatibility.attachApplication(app);"];
    [adapted replaceOccurrencesOfString:application withString:retinaApplication
                               options:0 range:NSMakeRange(0, adapted.length)];
    return [adapted dataUsingEncoding:NSUTF8StringEncoding];
}
