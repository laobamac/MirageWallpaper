#import "RMFontManager.h"
#import "RMLog.h"
#import <CoreText/CoreText.h>

@implementation RMFontManager {
    NSMutableSet<NSString *> *_registered;              // absolute file paths (UPPER)
    NSMutableDictionary<NSString *, NSString *> *_alias;// user-name UPPER → resolved PostScript name
}

+ (instancetype)shared {
    static RMFontManager *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [RMFontManager new]; });
    return s;
}

- (instancetype)init {
    if ((self = [super init])) {
        _registered = [NSMutableSet set];
        _alias = [NSMutableDictionary dictionary];
    }
    return self;
}

// Record all possible lookup keys for a descriptor. Rainmeter skins may
// specify FontFace as: the family, display, PostScript, or a filename-style
// alias such as "nasalization rg" — accept them all.
- (void)recordDescriptor:(CTFontDescriptorRef)desc
                fileName:(NSString *)fileName {
    NSString *fam = CFBridgingRelease(CTFontDescriptorCopyAttribute(desc, kCTFontFamilyNameAttribute));
    NSString *nm  = CFBridgingRelease(CTFontDescriptorCopyAttribute(desc, kCTFontNameAttribute));
    NSString *dp  = CFBridgingRelease(CTFontDescriptorCopyAttribute(desc, kCTFontDisplayNameAttribute));
    // Prefer the PostScript name so NSFont fontWithName: succeeds.
    NSString *canonical = nm ?: dp ?: fam;
    if (canonical.length == 0) return;
    void (^add)(NSString *) = ^(NSString *k){
        if (k.length == 0) return;
        _alias[k.uppercaseString] = canonical;
    };
    add(fam);
    add(nm);
    add(dp);
    add(fileName.stringByDeletingPathExtension);
    // Common alias forms authors write:
    if (fam.length) {
        add([fam stringByAppendingString:@" rg"]);
        add([fam stringByAppendingString:@" Rg"]);
        add([fam stringByAppendingString:@" Regular"]);
    }
}

- (void)registerFontsInDirectory:(NSString *)directory {
    if (directory.length == 0) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:directory isDirectory:&isDir] || !isDir) return;

    NSDirectoryEnumerator *en = [fm enumeratorAtPath:directory];
    for (NSString *rel in en) {
        NSString *ext = rel.pathExtension.lowercaseString;
        if (![ext isEqualToString:@"ttf"] && ![ext isEqualToString:@"otf"] &&
            ![ext isEqualToString:@"ttc"]) continue;
        NSString *full = [directory stringByAppendingPathComponent:rel];
        NSString *key = full.uppercaseString;
        if ([_registered containsObject:key]) continue;
        [_registered addObject:key];

        NSURL *url = [NSURL fileURLWithPath:full];
        CFErrorRef err = NULL;
        BOOL ok = CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,
                                                    kCTFontManagerScopeProcess, &err);
        if (err) CFRelease(err);
        if (!ok) RMLogDebug(@"font register skipped: %@", full);

        // Record aliases even when registration is a no-op (already registered).
        CFArrayRef arr = CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)url);
        if (arr) {
            for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
                CTFontDescriptorRef d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(arr, i);
                [self recordDescriptor:d fileName:rel.lastPathComponent];
            }
            CFRelease(arr);
        }
    }
}

- (NSFont *)fontWithFace:(nullable NSString *)face
                    size:(CGFloat)size
                    bold:(BOOL)bold
                  italic:(BOOL)italic {
    if (size <= 0) size = 12;
    NSFont *font = nil;

    NSString *trimmed = [face stringByTrimmingCharactersInSet:
                         [NSCharacterSet whitespaceCharacterSet]];
    if (trimmed.length > 0) {
        // 1. Alias table populated when the font file was registered.
        NSString *canonical = _alias[trimmed.uppercaseString];
        if (canonical.length) font = [NSFont fontWithName:canonical size:size];

        // 2. Fall through to plain family / full-name lookup for system fonts.
        if (font == nil) {
            NSFontManager *fm = NSFontManager.sharedFontManager;
            font = [fm fontWithFamily:trimmed traits:0 weight:5 size:size];
        }
        if (font == nil) font = [NSFont fontWithName:trimmed size:size];
    }
    if (font == nil) font = [NSFont systemFontOfSize:size];

    NSFontManager *fm = NSFontManager.sharedFontManager;
    // Only convert to Bold/Italic when the font family actually has that
    // variant. macOS's automatic synthesis over-thickens strokes for fonts
    // that were only shipped as Regular (e.g. "Nasalization Rg"), which
    // looks wrong compared to Windows/Rainmeter where the request silently
    // falls back to the plain style.
    if (bold) {
        NSFont *b = [fm convertFont:font toHaveTrait:NSBoldFontMask];
        if (b && [fm traitsOfFont:b] & NSBoldFontMask) font = b;
    }
    if (italic) {
        NSFont *it = [fm convertFont:font toHaveTrait:NSItalicFontMask];
        if (it && [fm traitsOfFont:it] & NSItalicFontMask) font = it;
    }
    return font;
}

@end
