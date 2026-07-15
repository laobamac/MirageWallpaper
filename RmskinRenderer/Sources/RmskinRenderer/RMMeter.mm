#import "RMMeter.h"
#import "RMConfigParser.h"
#import "RMMeasure.h"
#import "RMFontManager.h"
#import "RMLog.h"
#import <CoreImage/CoreImage.h>

#pragma mark - Helpers

static NSColor *RMParseColor(NSString *_Nullable s, NSColor *fallback) {
    if (s.length == 0) return fallback;
    NSArray<NSString *> *parts = [s componentsSeparatedByString:@","];
    if (parts.count >= 3) {
        // Components may be plain numbers or Rainmeter formulas, e.g.
        // "255,255,255,(255*0.8)". Evaluate each so semi-transparent fills
        // (the "frosted glass" look) resolve instead of collapsing to 0.
        CGFloat r = [RMConfigParser evaluateNumber:parts[0] default:0] / 255.0;
        CGFloat g = [RMConfigParser evaluateNumber:parts[1] default:0] / 255.0;
        CGFloat b = [RMConfigParser evaluateNumber:parts[2] default:0] / 255.0;
        CGFloat a = parts.count >= 4 ? [RMConfigParser evaluateNumber:parts[3] default:255] / 255.0 : 1.0;
        r = MAX(0, MIN(1, r)); g = MAX(0, MIN(1, g)); b = MAX(0, MIN(1, b)); a = MAX(0, MIN(1, a));
        return [NSColor colorWithSRGBRed:r green:g blue:b alpha:a];
    }
    // Hex RRGGBB / RRGGBBAA.
    NSString *hex = [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (hex.length == 6 || hex.length == 8) {
        unsigned int v = 0; NSScanner *sc = [NSScanner scannerWithString:hex];
        if ([sc scanHexInt:&v]) {
            if (hex.length == 6) {
                return [NSColor colorWithSRGBRed:((v>>16)&0xFF)/255.0
                                           green:((v>>8)&0xFF)/255.0
                                            blue:(v&0xFF)/255.0 alpha:1.0];
            } else {
                return [NSColor colorWithSRGBRed:((v>>24)&0xFF)/255.0
                                           green:((v>>16)&0xFF)/255.0
                                            blue:((v>>8)&0xFF)/255.0
                                           alpha:(v&0xFF)/255.0];
            }
        }
    }
    return fallback;
}

static NSString *RMNormalizePath(NSString *_Nullable p) {
    if (p.length == 0) return p ?: @"";
    return [p stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
}

// Resolve an image path: absolute paths are used as-is; relative paths
// ("My Computer.png") are looked up first in the skin config directory
// (#CURRENTPATH#) and then in the config @Resources folder (#@#).
static NSString *RMResolveImagePath(NSString *_Nullable raw, RMConfigParser *parser) {
    NSString *p = RMNormalizePath(raw);
    if (p.length == 0) return @"";
    NSFileManager *fm = [NSFileManager defaultManager];

    // Try `path` as-is, and — when it has no usable image extension or the file
    // is missing — with common image extensions appended. Some skins omit the
    // ".png" suffix (e.g. ImageName=#@#Image\Icons\16\ALeft).
    NSArray<NSString *> *exts = @[@"png", @"jpg", @"jpeg", @"bmp", @"gif", @"tif", @"tiff", @"ico"];
    NSString *(^resolveIn)(NSString *) = ^NSString *(NSString *base) {
        if ([fm fileExistsAtPath:base]) return base;
        NSString *ext = base.pathExtension.lowercaseString;
        if (![exts containsObject:ext]) {
            for (NSString *e in exts) {
                NSString *cand = [base stringByAppendingPathExtension:e];
                if ([fm fileExistsAtPath:cand]) return cand;
            }
        }
        return nil;
    };

    if (p.isAbsolutePath) {
        NSString *r = resolveIn(p);
        return r ?: p;
    }
    NSString *cur = parser.currentPath;
    if (cur.length) {
        NSString *r = resolveIn([cur stringByAppendingPathComponent:p]);
        if (r) return r;
    }
    NSString *res = parser.resourcesPath;
    if (res.length) {
        NSString *r = resolveIn([res stringByAppendingPathComponent:p]);
        if (r) return r;
    }
    // Fall back to config-dir join even if missing, for logging clarity.
    return cur.length ? [cur stringByAppendingPathComponent:p] : p;
}

// Multiply-tint an image (Rainmeter ImageTint semantics: out.rgba = src.rgba *
// tint.rgba / 255). ImageTint=255,255,255,200 keeps the original colours and
// scales alpha to 200/255; a coloured tint recolours a greyscale source. This
// is very different from a solid overlay (SourceAtop), which would replace
// the pixel colours entirely.
static NSImage *RMTintImage(NSImage *src, NSColor *tint) {
    if (src == nil || tint == nil) return src;
    NSSize sz = src.size;
    if (sz.width <= 0 || sz.height <= 0) return src;

    NSColor *srgb = [tint colorUsingColorSpace:NSColorSpace.sRGBColorSpace] ?: tint;
    CGFloat tr = srgb.redComponent, tg = srgb.greenComponent, tb = srgb.blueComponent, ta = srgb.alphaComponent;

    CIImage *ci = [CIImage imageWithData:[src TIFFRepresentation]];
    if (ci == nil) return src;
    CIFilter *m = [CIFilter filterWithName:@"CIColorMatrix"];
    [m setValue:ci forKey:kCIInputImageKey];
    [m setValue:[CIVector vectorWithX:tr Y:0 Z:0 W:0] forKey:@"inputRVector"];
    [m setValue:[CIVector vectorWithX:0 Y:tg Z:0 W:0] forKey:@"inputGVector"];
    [m setValue:[CIVector vectorWithX:0 Y:0 Z:tb W:0] forKey:@"inputBVector"];
    [m setValue:[CIVector vectorWithX:0 Y:0 Z:0 W:ta] forKey:@"inputAVector"];
    CIImage *out = m.outputImage;
    if (out == nil) return src;
    NSCIImageRep *rep = [NSCIImageRep imageRepWithCIImage:out];
    NSImage *result = [[NSImage alloc] initWithSize:sz];
    [result addRepresentation:rep];
    return result;
}

static double RMRelativeValue(RMMeasure *m) {
    if (m == nil) return 0;
    double range = m.maxValue - m.minValue;
    if (range <= 0) return 0;
    double r = (m.value - m.minValue) / range;
    return r < 0 ? 0 : (r > 1 ? 1 : r);
}

#pragma mark - Base

@interface RMMeter ()
@property (nonatomic, strong) NSColor *solidColor;
- (void)readSubclassOptions;
- (void)fillBackgroundIfNeeded;
- (NSString *)measureStringAtIndex:(int)idx;
@end

// Concrete meter classes.
@interface RMMeterImage     : RMMeter @end
@interface RMMeterString    : RMMeter @end
@interface RMMeterBar       : RMMeter @end
@interface RMMeterLine      : RMMeter @end
@interface RMMeterHistogram : RMMeter @end
@interface RMMeterRotator   : RMMeter @end
@interface RMMeterRoundLine : RMMeter @end
@interface RMMeterShape     : RMMeter @end
@interface RMMeterBitmap    : RMMeter @end
@interface RMMeterButton    : RMMeter @end

@implementation RMMeter

+ (nullable RMMeter *)meterWithType:(NSString *)type
                               name:(NSString *)name
                             parser:(RMConfigParser *)parser {
    NSString *t = type.lowercaseString;
    Class cls;
    if ([t isEqualToString:@"image"])          cls = [RMMeterImage class];
    else if ([t isEqualToString:@"string"])    cls = [RMMeterString class];
    else if ([t isEqualToString:@"bar"])       cls = [RMMeterBar class];
    else if ([t isEqualToString:@"line"])      cls = [RMMeterLine class];
    else if ([t isEqualToString:@"histogram"]) cls = [RMMeterHistogram class];
    else if ([t isEqualToString:@"rotator"])   cls = [RMMeterRotator class];
    else if ([t isEqualToString:@"roundline"]) cls = [RMMeterRoundLine class];
    else if ([t isEqualToString:@"shape"])     cls = [RMMeterShape class];
    else if ([t isEqualToString:@"bitmap"])    cls = [RMMeterBitmap class];
    else if ([t isEqualToString:@"button"])    cls = [RMMeterButton class];
    else return nil;

    RMMeter *m = [[cls alloc] init];
    m.name = name;
    m.parser = parser;
    return m;
}

- (NSRect)frame { return NSMakeRect(self.x, self.y, self.w, self.h); }

- (void)readOptions {
    RMConfigParser *cp = self.parser;

    NSString *xs = [cp readString:self.name key:@"X" default:@"0"];
    NSString *ys = [cp readString:self.name key:@"Y" default:@"0"];
    [self parsePosition:xs isX:YES];
    [self parsePosition:ys isX:NO];

    self.w = [cp readDouble:self.name key:@"W" default:0];
    self.h = [cp readDouble:self.name key:@"H" default:0];
    self.hidden = [cp readBool:self.name key:@"Hidden" default:NO];
    self.antiAlias = [cp readBool:self.name key:@"AntiAlias" default:YES];
    self.group = [cp readString:self.name key:@"Group" default:nil];
    self.solidColor = RMParseColor([cp readString:self.name key:@"SolidColor" default:nil], nil);

    // MeasureName / MeasureName2 ... MeasureNameN
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    NSString *first = [cp readString:self.name key:@"MeasureName" default:nil];
    if (first.length) [names addObject:first];
    for (int i = 2; i <= 16; i++) {
        NSString *k = [NSString stringWithFormat:@"MeasureName%d", i];
        NSString *v = [cp readString:self.name key:k default:nil];
        if (v.length) [names addObject:v]; else if (i > 2 && first == nil) break;
    }
    self.measureNames = names;

    self.leftMouseUpAction  = [cp readString:self.name key:@"LeftMouseUpAction"  default:nil];
    self.rightMouseUpAction = [cp readString:self.name key:@"RightMouseUpAction" default:nil];
    self.middleMouseUpAction = [cp readString:self.name key:@"MiddleMouseUpAction" default:nil];
    self.mouseOverAction    = [cp readString:self.name key:@"MouseOverAction"    default:nil];
    self.mouseLeaveAction   = [cp readString:self.name key:@"MouseLeaveAction"   default:nil];
    self.container          = [cp readString:self.name key:@"Container"          default:nil];

    [self readSubclassOptions];
}

- (void)parsePosition:(NSString *)s isX:(BOOL)isX {
    NSString *v = [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    BOOL relative = NO, br = NO;
    if (v.length > 0) {
        unichar last = [v characterAtIndex:v.length - 1];
        if (last == 'r') { relative = YES; br = NO;  v = [v substringToIndex:v.length - 1]; }
        else if (last == 'R') { relative = YES; br = YES; v = [v substringToIndex:v.length - 1]; }
    }
    double num = [RMConfigParser evaluateNumber:v default:0];
    if (isX) { self.x = num; self.authoredX = num; self.relativeX = relative; self.relativeXBR = br; self.rawX = s; }
    else     { self.y = num; self.authoredY = num; self.relativeY = relative; self.relativeYBR = br; self.rawY = s; }
}

// Reset live position to the authored value so alignment/relative offsets do
// not accumulate across ticks.
- (void)resetToAuthoredPosition {
    self.x = self.authoredX;
    self.y = self.authoredY;
}

// Resolve relative position given the previous meter (called during layout).
// Rainmeter semantics:
//   x=Nr  → prev.x + N       (TL: relative to previous top-left)
//   x=NR  → prev.x + prev.w + N   (BR: relative to previous bottom-right)
- (void)resolvePositionWithPrevious:(nullable RMMeter *)prev {
    if (prev == nil) return;
    if (self.relativeX) self.x = prev.x + (self.relativeXBR ? prev.w : 0) + self.authoredX;
    if (self.relativeY) self.y = prev.y + (self.relativeYBR ? prev.h : 0) + self.authoredY;
}

- (void)readSubclassOptions { /* override */ }
- (void)prepare { /* override */ }
- (void)draw { /* override */ }

// TransformationMatrix: affine transform applied to the meter before drawing.
// Format: "a;b;c;d;tx;ty" (6 semicolon-separated numbers).
// Applied via NSAffineTransform to the entire drawing context.
- (NSAffineTransform *)transformationMatrix {
    NSArray *parts = [[self.parser readString:self.name key:@"TransformationMatrix" default:nil]
                      componentsSeparatedByString:@";"];
    if (parts.count < 6) return nil;
    double a  = [parts[0] doubleValue];
    double b  = [parts[1] doubleValue];
    double c  = [parts[2] doubleValue];
    double d  = [parts[3] doubleValue];
    double tx = [parts[4] doubleValue];
    double ty = [parts[5] doubleValue];
    if (a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0) return nil;
    NSAffineTransform *tr = [NSAffineTransform transform];
    NSAffineTransformStruct st = { a, b, c, d, tx, ty };
    [tr setTransformStruct:st];
    return tr;
}

- (void)drawWithTransform:(NSAffineTransform *)tr block:(void(^)(void))block {
    if (tr) {
        NSGraphicsContext *gc = [NSGraphicsContext currentContext];
        [gc saveGraphicsState];
        [tr concat];
        block();
        [gc restoreGraphicsState];
    } else {
        block();
    }
}

// Build the display text for a bound measure index (1-based).
- (NSString *)measureStringAtIndex:(int)idx {
    int i = idx - 1;
    if (i < 0 || i >= (int)self.measures.count) return @"";
    RMMeasure *m = self.measures[i];
    RMConfigParser *cp = self.parser;
    BOOL autoScale  = [cp readBool:self.name key:@"AutoScale" default:NO];
    BOOL percentual = [cp readBool:self.name key:@"Percentual" default:NO];
    int decimals    = [cp readInt:self.name key:@"NumberOfDecimals" default:-1];
    double scale    = [cp readDouble:self.name key:@"Scale" default:1];
    return [m displayStringAutoScale:autoScale decimals:decimals percentual:percentual scale:scale];
}

- (void)fillBackgroundIfNeeded {
    if (self.solidColor && self.w > 0 && self.h > 0) {
        [self.solidColor setFill];
        NSRectFillUsingOperation(self.frame, NSCompositingOperationSourceOver);
    }
}

@end

#pragma mark - Image

@implementation RMMeterImage {
    NSString *_imagePath;
    NSImage *_image;
    NSString *_loadedPath;
    CGFloat _alpha;
    BOOL _greyscale;
    BOOL _preserveAspect;
    NSColor *_tint;
}
- (void)readSubclassOptions {
    _imagePath = [self.parser readString:self.name key:@"ImageName" default:nil];
    _alpha = [self.parser readDouble:self.name key:@"ImageAlpha" default:255] / 255.0;
    _greyscale = [self.parser readBool:self.name key:@"Greyscale" default:NO];
    _preserveAspect = [self.parser readBool:self.name key:@"PreserveAspectRatio" default:NO];
    _tint = RMParseColor([self.parser readString:self.name key:@"ImageTint" default:nil], nil);
    _loadedPath = nil;   // force reload when options change (e.g. !SetOption)
}
- (void)loadIfNeeded {
    if (_imagePath.length == 0) return;
    NSString *resolved = RMResolveImagePath(_imagePath, self.parser);
    if ([_loadedPath isEqualToString:resolved] && _image) return;
    _loadedPath = resolved;
    _image = [[NSImage alloc] initWithContentsOfFile:resolved];
    if (_image == nil) { RMLogDebug(@"image not found: %@", resolved); return; }
    if (_greyscale) {
        CIImage *ci = [CIImage imageWithData:[_image TIFFRepresentation]];
        CIFilter *f = [CIFilter filterWithName:@"CIPhotoEffectMono"];
        [f setValue:ci forKey:kCIInputImageKey];
        CIImage *out = f.outputImage;
        if (out) {
            // NSCIImageRep.size returns the CIImage extent (pixels), not
            // the point dimensions of the original NSImage.  For a @2x
            // asset the extent may be 32×32 while the intended display size
            // is 16×16.  Always use the original `_image.size` so the
            // greyscale version stays at the same point size — otherwise
            // the meter auto-width/height doubles and the widget grows to
            // twice its intended size.
            NSCIImageRep *rep = [NSCIImageRep imageRepWithCIImage:out];
            NSSize originalPointSize = _image.size;
            NSImage *g = [[NSImage alloc] initWithSize:originalPointSize];
            [g addRepresentation:rep];
            _image = g;
        }
    }
    if (_tint) _image = RMTintImage(_image, _tint);
}
- (void)prepare {
    [self loadIfNeeded];
    if (_image) {
        if (self.w <= 0) self.w = _image.size.width;
        if (self.h <= 0) self.h = _image.size.height;
    }

}
- (void)draw {
    [self fillBackgroundIfNeeded];
    if (_image == nil) return;
    NSRect dst = self.frame;
    if (_preserveAspect && _image.size.width > 0 && _image.size.height > 0) {
        CGFloat s = MIN(dst.size.width / _image.size.width, dst.size.height / _image.size.height);
        CGFloat nw = _image.size.width * s, nh = _image.size.height * s;
        dst = NSMakeRect(dst.origin.x + (dst.size.width - nw) / 2,
                         dst.origin.y + (dst.size.height - nh) / 2, nw, nh);
    }
    [_image drawInRect:dst fromRect:NSZeroRect
             operation:NSCompositingOperationSourceOver fraction:_alpha respectFlipped:YES hints:nil];
}
@end

#pragma mark - String

@implementation RMMeterString {
    NSString *_text;
    NSString *_prefix, *_postfix;
    NSString *_align;
    NSString *_case;
    NSString *_effect;
    NSColor *_fontColor;
    NSColor *_effectColor;
    NSString *_face;
    CGFloat _fontSize;
    BOOL _bold, _italic;
    CGFloat _charSpacing;
    int _clipString; // 0=none, 1=clip to W, 2=clip+wrap to W,H
    NSString *_renderedText;
    NSSize _measured;
    NSPoint _drawOrigin;
    // InlineSetting support: a segment-based text renderer.
    NSMutableArray<NSDictionary *> *_inlineSegments; // [{range, font, color, kern}]
}

// Parse InlineSetting/InlineSetting2/...N into per-segment attributes.
// Supports: Face | FontName, Size | N, Color | R,G,B,A, Weight | Bold/Normal.
- (void)parseInlineSettings {
    _inlineSegments = [NSMutableArray array];
    RMConfigParser *cp = self.parser;
    RMIniSection *sec = [cp.ini sectionNamed:self.name];
    if (sec == nil) return;

    // Collect ranges: InlinePattern=N records the search pattern, then
    // InlineSetting/InlineSetting2 apply attributes to that range.
    NSMutableArray<NSDictionary *> *pending = [NSMutableArray array];
    NSString *fullText = self->_renderedText;

    for (int i = 1; i <= 64; i++) {
        NSString *settingKey = (i == 1) ? @"InlineSetting" : [NSString stringWithFormat:@"InlineSetting%d", i];
        NSString *patternKey = (i == 1) ? @"InlinePattern" : [NSString stringWithFormat:@"InlinePattern%d", i];
        NSString *rawSetting = [sec valueForKey:settingKey];
        if (rawSetting.length == 0) {
            if (i > 1) break;
            continue;
        }
        NSString *expanded = [cp expand:rawSetting];
        NSArray *parts = [expanded componentsSeparatedByString:@"|"];
        if (parts.count < 2) continue;
        NSString *attr = [[parts[0] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] lowercaseString];
        NSString *val  = [parts[1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];

        // Pattern determines which text segment gets styled.
        NSString *rawPattern = [sec valueForKey:patternKey];
        NSString *pattern = rawPattern.length ? [cp expand:rawPattern] : nil;

        NSMutableDictionary *seg = [NSMutableDictionary dictionary];
        seg[@"pattern"] = pattern ?: @"";
        seg[@"fullText"] = fullText ?: @"";
        if ([attr isEqualToString:@"face"]) {
            seg[@"face"] = val;
        } else if ([attr isEqualToString:@"size"]) {
            seg[@"size"] = @([val doubleValue]);
        } else if ([attr isEqualToString:@"color"]) {
            NSColor *c = RMParseColor(val, nil);
            if (c) seg[@"color"] = c;
        } else if ([attr isEqualToString:@"weight"]) {
            seg[@"bold"] = @([val.lowercaseString hasPrefix:@"bold"] || [val integerValue] >= 700);
        } else if ([attr isEqualToString:@"italic"]) {
            seg[@"italic"] = @([val.lowercaseString isEqualToString:@"italic"]);
        } else if ([attr isEqualToString:@"characterspacing"]) {
            seg[@"kern"] = @([val doubleValue]);
        } else if ([attr isEqualToString:@"shadow"]) {
            seg[@"shadowColor"] = RMParseColor(val, [NSColor blackColor]);
        } else if ([attr isEqualToString:@"strikethrough"]) {
            seg[@"strikethrough"] = @YES;
        } else if ([attr isEqualToString:@"underline"]) {
            seg[@"underline"] = @YES;
        }
        if (seg.count > 3) [pending addObject:seg]; // has at least pattern+fullText+1 attr
    }
    _inlineSegments = pending;
}

- (void)readSubclassOptions {
    RMConfigParser *cp = self.parser;
    _text = [cp readString:self.name key:@"Text" default:nil];
    _prefix = [cp readString:self.name key:@"Prefix" default:@""];
    _postfix = [cp readString:self.name key:@"Postfix" default:@""];
    _align = ([cp readString:self.name key:@"StringAlign" default:@"Left"]).lowercaseString;
    _case = ([cp readString:self.name key:@"StringCase" default:@"None"]).lowercaseString;
    _clipString = [cp readInt:self.name key:@"ClipString" default:0];
    _effect = ([cp readString:self.name key:@"StringEffect" default:@"None"]).lowercaseString;
    _fontColor = RMParseColor([cp readString:self.name key:@"FontColor" default:nil],
                              [NSColor whiteColor]);
    _effectColor = RMParseColor([cp readString:self.name key:@"FontEffectColor" default:nil],
                                [NSColor blackColor]);
    _face = [cp readString:self.name key:@"FontFace" default:nil];
    _fontSize = [cp readDouble:self.name key:@"FontSize" default:10];
    NSString *style = ([cp readString:self.name key:@"StringStyle" default:@"Normal"]).lowercaseString;
    _bold = [style containsString:@"bold"];
    _italic = [style containsString:@"italic"];
    // Basic CharacterSpacing (legacy fallback; InlineSetting is preferred).
    NSString *inl = [cp readString:self.name key:@"InlineSetting" default:nil];
    if ([inl.lowercaseString containsString:@"characterspacing"]) {
        NSArray *parts = [inl componentsSeparatedByString:@"|"];
        if (parts.count >= 2) _charSpacing = [parts[1] doubleValue];
    }
}
// Process C-style escape sequences in a String meter's Text value. Rainmeter
// skins include icons as `\xe897` (Material Icons character codes), C-style
// hex escapes that NSString would otherwise display literally. Translate
// the common escapes here so the resulting NSString contains the actual
// unicode characters.
static NSString *RMProcessStringEscapes(NSString *raw) {
    if (raw.length == 0) return raw;
    NSMutableString *out = [NSMutableString stringWithCapacity:raw.length];
    NSUInteger i = 0, n = raw.length;
    while (i < n) {
        unichar c = [raw characterAtIndex:i];
        if (c != '\\') { [out appendFormat:@"%C", c]; i++; continue; }
        // Escape sequence; need at least one more char.
        if (i + 1 >= n) { [out appendString:@"\\"]; i++; continue; }
        unichar next = [raw characterAtIndex:i + 1];
        switch (next) {
            case '\\': [out appendString:@"\\"]; i += 2; break;
            case 'n':  [out appendString:@"\n"]; i += 2; break;
            case 'r':  [out appendString:@"\r"]; i += 2; break;
            case 't':  [out appendString:@"\t"]; i += 2; break;
            case 'x':
            case 'X': {
                // \x — read up to 4 consecutive hex digits (C semantics).
                NSUInteger j = i + 2;
                NSUInteger hexEnd = j;
                while (hexEnd < n && hexEnd - j < 4) {
                    unichar hx = [raw characterAtIndex:hexEnd];
                    if (!((hx >= '0' && hx <= '9') || (hx >= 'a' && hx <= 'f') || (hx >= 'A' && hx <= 'F'))) break;
                    hexEnd++;
                }
                if (hexEnd > j) {
                    NSString *hex = [raw substringWithRange:NSMakeRange(j, hexEnd - j)];
                    unsigned v = 0;
                    NSScanner *sc = [NSScanner scannerWithString:hex];
                    if ([sc scanHexInt:&v]) {
                        [out appendFormat:@"%C", (unichar)v];
                        i = hexEnd;
                        continue;
                    }
                }
                [out appendFormat:@"%C", c]; i++;
                break;
            }
            case 'u': {
                // \uNNNN — exactly 4 hex digits follow.
                if (i + 5 < n) {
                    NSString *hex = [raw substringWithRange:NSMakeRange(i + 2, 4)];
                    unsigned v = 0;
                    NSScanner *sc = [NSScanner scannerWithString:hex];
                    if ([sc scanHexInt:&v] && sc.scanLocation == 4) {
                        [out appendFormat:@"%C", (unichar)v];
                        i += 6;
                        continue;
                    }
                }
                [out appendFormat:@"%C", c]; i++;
                break;
            }
            default:
                // Unknown escape: keep the literal text and the backslash.
                [out appendFormat:@"%C%C", c, next]; i += 2; break;
        }
    }
    return out;
}

- (NSString *)composeText {
    NSString *t = _text ? RMProcessStringEscapes(_text) : (self.measureNames.count ? @"%1" : @"");
    for (int i = (int)self.measures.count; i >= 1; i--) {
        NSString *tok = [NSString stringWithFormat:@"%%%d", i];
        t = [t stringByReplacingOccurrencesOfString:tok withString:[self measureStringAtIndex:i]];
    }
    // Rainmeter also supports "%MeasureName%" placeholders: any substring of
    // the form %Name% is replaced with the value of the bound measure whose
    // name is "Name". Used by skins such as the AWESOME profile widget, which
    // writes `Text=%USERNAME%` with `MeasureName=MeasureUserName`.
    if (self.measures.count > 0 && t.length > 0) {
        NSRange pct = [t rangeOfString:@"%"];
        while (pct.location != NSNotFound) {
            NSRange end = [t rangeOfString:@"%" options:0
                                      range:NSMakeRange(pct.location + 1, t.length - pct.location - 1)];
            if (end.location == NSNotFound) break;
            NSString *name = [t substringWithRange:NSMakeRange(pct.location + 1, end.location - pct.location - 1)];
            // Skip pure-numeric %N tokens (already handled above) and any
            // name with characters that aren't valid in a section header.
            BOOL nameOk = name.length > 0;
            for (NSUInteger i = 0; i < name.length; i++) {
                unichar c = [name characterAtIndex:i];
                if (!(isalnum((int)c) || c == '_' || c == '-')) { nameOk = NO; break; }
            }
            if (nameOk && [name stringByTrimmingCharactersInSet:NSCharacterSet.decimalDigitCharacterSet].length == 0) {
                // Pure-digit name is the %N form, already handled above; skip
                // the name-based lookup so we don't try to resolve "1" as a
                // measure name. (The misplaced `!` in the previous version
                // made this condition fire for every non-pure-digit name,
                // which is why %USERNAME% was never being substituted.)
                nameOk = NO;
            }
            if (nameOk) {
                NSString *val = nil;
                if (self.parser.measureStringResolver) {
                    val = self.parser.measureStringResolver(name);
                }
                if (val != nil) {
                    t = [t stringByReplacingCharactersInRange:NSMakeRange(pct.location, end.location - pct.location + 1)
                                                     withString:val];
                    pct = [t rangeOfString:@"%" options:0
                                     range:NSMakeRange(pct.location + val.length, t.length - (pct.location + val.length))];
                    continue;
                }
            }
            pct = [t rangeOfString:@"%" options:0 range:NSMakeRange(end.location + 1, t.length - end.location - 1)];
        }
    }
    t = [NSString stringWithFormat:@"%@%@%@", _prefix ?: @"", t, _postfix ?: @""];
    if ([_case isEqualToString:@"upper"]) t = t.uppercaseString;
    else if ([_case isEqualToString:@"lower"]) t = t.lowercaseString;
    else if ([_case isEqualToString:@"proper"]) t = t.capitalizedString;
    return t;
}
- (NSDictionary *)attributes {
    NSFont *font = [[RMFontManager shared] fontWithFace:_face size:_fontSize bold:_bold italic:_italic];
    // NOTE: we do NOT set paragraph-style alignment here. Rainmeter's
    // StringAlign shifts the whole text box relative to the (X,Y) anchor
    // — it is not intra-box justification. drawInRect: with a paragraph
    // alignment plus a wider-than-text box would justify the glyphs inside
    // the box and defeat the anchor shift we compute in `prepare`.
    NSMutableDictionary *attrs = [@{
        NSFontAttributeName: font,
        NSForegroundColorAttributeName: _fontColor,
    } mutableCopy];
    if (_charSpacing != 0) attrs[NSKernAttributeName] = @(_charSpacing);
    return attrs;
}
- (void)prepare {
    _renderedText = [self composeText];
    [self parseInlineSettings];
    NSDictionary *attrs = [self attributes];
    _measured = [_renderedText sizeWithAttributes:attrs];
    if (_inlineSegments.count > 0) {
        // When inline segments are active, measure with default attrs
        // and adjust W/H for inline-size segments.
        for (NSDictionary *seg in _inlineSegments) {
            NSNumber *sz = seg[@"size"];
            if (sz) {
                NSFont *segFont = [[RMFontManager shared] fontWithFace:seg[@"face"] ?: _face
                                                                  size:sz.doubleValue
                                                                  bold:[seg[@"bold"] boolValue]
                                                                italic:[seg[@"italic"] boolValue]];
                NSDictionary *segAttrs = @{NSFontAttributeName: segFont};
                NSSize segSize = [_renderedText sizeWithAttributes:segAttrs];
                _measured = NSMakeSize(MAX(_measured.width, segSize.width),
                                       MAX(_measured.height, segSize.height));
            }
        }
    }
    if (self.w <= 0) self.w = ceil(_measured.width);
    if (self.h <= 0) self.h = ceil(_measured.height);

    // Compute the aligned draw origin ONCE per tick from the (stable) anchor.
    // self.x/self.y stay at the authored anchor so nothing accumulates.
    NSSize sz = _measured;
    CGFloat drawX = self.x, drawY = self.y;
    // Rainmeter StringAlign semantics. The meter's (X, Y) is the anchor point;
    // the text box is shifted so the specified corner/edge coincides with it.
    //   X-only (Y stays top): "Left" (default), "Center", "Right"
    //   Combined (Y prefix + X suffix):
    //     "TopLeft"   "TopCenter"   "TopRight"
    //     "CenterLeft" "Center"     "CenterRight"
    //     "BottomLeft" "BottomCenter" "BottomRight"
    // The previous hasPrefix:@"center"/hasSuffix:@"center" approach only worked
    // for the bare X-only values — for any combined value like "TopRight" the
    // "right"/"center" token sits in the *middle* of the string, so neither
    // check fired and the X/Y shift was silently dropped, causing the
    // misaligned text visible in the rendered skin (e.g. "C" stuck at the
    // left of its frame instead of the right, "Tmrw:" pinned to the top of
    // its frame instead of the bottom).
    NSString *alignUpper = _align.uppercaseString;
    BOOL centerX = NO, centerY = NO;
    BOOL rightX  = NO, bottomY = NO;
    if ([alignUpper isEqualToString:@"LEFT"]) {
        // X-Left, Y-Top (defaults).
    } else if ([alignUpper isEqualToString:@"RIGHT"]) {
        rightX = YES;
    } else if ([alignUpper isEqualToString:@"CENTER"]) {
        // Bare "Center" means X-Center, Y-Top (same as "TopCenter"). The
        // combined "Center" is handled below as a real two-axis case.
        centerX = YES;
    } else {
        // Combined values: parse Y from the prefix, X from the suffix.
        if ([alignUpper hasPrefix:@"TOP"]) {
            // Y-Top.
        } else if ([alignUpper hasPrefix:@"CENTER"]) {
            centerY = YES;
        } else if ([alignUpper hasPrefix:@"BOTTOM"]) {
            bottomY = YES;
        }
        if ([alignUpper hasSuffix:@"LEFT"]) {
            // X-Left.
        } else if ([alignUpper hasSuffix:@"CENTER"]) {
            centerX = YES;
        } else if ([alignUpper hasSuffix:@"RIGHT"]) {
            rightX = YES;
        }
    }
    if (centerX) drawX = self.x - sz.width / 2.0;
    else if (rightX) drawX = self.x - sz.width;
    if (centerY) drawY = self.y - sz.height / 2.0;
    else if (bottomY) drawY = self.y - sz.height;
    _drawOrigin = NSMakePoint(drawX, drawY);
}

// Match a pattern against the rendered text. Returns the range of the first
// match, or NSNotFound. Patterns are plain substrings (regular expression
// support could be added for InlinePattern with RegExp).
static NSRange RMFindPatternInText(NSString *text, NSString *_Nullable pattern, NSRange searchRange) {
    if (pattern.length == 0) return searchRange; // "whole text"
    return [text rangeOfString:pattern options:0 range:searchRange];
}

- (void)draw {
    [self fillBackgroundIfNeeded];
    if (_renderedText.length == 0) return;
    NSDictionary *baseAttrs = [self attributes];
    NSSize sz = _measured;

    // If no inline segments, draw the whole text with base attributes.
    if (_inlineSegments.count == 0) {
        NSRect box = NSMakeRect(_drawOrigin.x, _drawOrigin.y, ceil(sz.width) + 1, sz.height);
        if ([_effect isEqualToString:@"shadow"]) {
            NSMutableDictionary *e = [baseAttrs mutableCopy];
            e[NSForegroundColorAttributeName] = _effectColor;
            [_renderedText drawInRect:NSOffsetRect(box, 1, 1) withAttributes:e];
        } else if ([_effect isEqualToString:@"border"]) {
            NSMutableDictionary *e = [baseAttrs mutableCopy];
            e[NSForegroundColorAttributeName] = _effectColor;
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    if (dx || dy) [_renderedText drawInRect:NSOffsetRect(box, dx, dy) withAttributes:e];
        }
        [_renderedText drawInRect:box withAttributes:baseAttrs];
        return;
    }

    // Segment-based rendering: draw each segment of text with its own attributes.
    // We accumulate x-position as we draw from left to right.
    CGFloat cursorX = _drawOrigin.x;
    CGFloat lineY = _drawOrigin.y;
    NSUInteger pos = 0;
    NSUInteger totalLen = _renderedText.length;

    while (pos < totalLen) {
        // Find the next segment that applies at or after `pos`.
        NSDictionary *bestSeg = nil;
        NSRange bestRange = NSMakeRange(NSNotFound, 0);
        for (NSDictionary *seg in _inlineSegments) {
            NSString *pat = seg[@"pattern"] ?: @"";
            NSRange r;
            if (pat.length == 0) {
                // Whole-text segment: applies everywhere.
                r = NSMakeRange(pos, totalLen - pos);
            } else {
                r = RMFindPatternInText(_renderedText, pat, NSMakeRange(pos, totalLen - pos));
            }
            if (r.location != NSNotFound && (bestSeg == nil || r.location < bestRange.location)) {
                bestSeg = seg;
                bestRange = r;
            }
        }

        if (bestSeg == nil || bestRange.location == NSNotFound || bestRange.location > pos) {
            // Gap before the first segment: draw with base attributes.
            NSUInteger gapEnd = bestSeg ? bestRange.location : totalLen;
            if (gapEnd > pos) {
                NSString *gapText = [_renderedText substringWithRange:NSMakeRange(pos, gapEnd - pos)];
                NSSize gapSize = [gapText sizeWithAttributes:baseAttrs];
                NSRect gapBox = NSMakeRect(cursorX, lineY, ceil(gapSize.width), gapSize.height);
                [gapText drawInRect:gapBox withAttributes:baseAttrs];
                cursorX += ceil(gapSize.width);
                pos = gapEnd;
            }
            if (bestSeg == nil) break;
        }

        // Draw the matched segment.
        NSString *segText = [_renderedText substringWithRange:bestRange];

        // Build segment attributes by overriding base with InlineSetting values.
        NSMutableDictionary *segAttrs = [baseAttrs mutableCopy];
        NSString *face = bestSeg[@"face"];
        NSNumber *szN  = bestSeg[@"size"];
        BOOL bold = bestSeg[@"bold"] ? [bestSeg[@"bold"] boolValue] : _bold;
        BOOL italic = bestSeg[@"italic"] ? [bestSeg[@"italic"] boolValue] : _italic;
        CGFloat fontSize = szN ? szN.doubleValue : _fontSize;
        NSFont *font = [[RMFontManager shared] fontWithFace:face ?: _face size:fontSize bold:bold italic:italic];
        segAttrs[NSFontAttributeName] = font;
        NSColor *color = bestSeg[@"color"];
        if (color) segAttrs[NSForegroundColorAttributeName] = color;
        NSNumber *kern = bestSeg[@"kern"];
        if (kern) segAttrs[NSKernAttributeName] = kern;
        if (bestSeg[@"strikethrough"]) segAttrs[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
        if (bestSeg[@"underline"]) segAttrs[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);

        // Shadow on this segment if specified.
        NSColor *shadowCol = bestSeg[@"shadowColor"];
        if (shadowCol) {
            NSMutableDictionary *shAttrs = [segAttrs mutableCopy];
            shAttrs[NSForegroundColorAttributeName] = shadowCol;
            NSRect shBox = NSMakeRect(cursorX + 1, lineY + 1, 0, 0);
            [segText drawAtPoint:shBox.origin withAttributes:shAttrs];
        }

        NSSize segSz = [segText sizeWithAttributes:segAttrs];
        NSRect segBox = NSMakeRect(cursorX, lineY, ceil(segSz.width), segSz.height);
        [segText drawInRect:segBox withAttributes:segAttrs];
        cursorX += ceil(segSz.width);
        pos = bestRange.location + bestRange.length;
    }
}
@end

#pragma mark - Bar

@implementation RMMeterBar {
    NSColor *_barColor;
    NSString *_orientation;
    BOOL _flip;
}
- (void)readSubclassOptions {
    _barColor = RMParseColor([self.parser readString:self.name key:@"BarColor" default:nil],
                             [NSColor whiteColor]);
    _orientation = ([self.parser readString:self.name key:@"BarOrientation" default:@"Vertical"]).lowercaseString;
    _flip = [self.parser readBool:self.name key:@"Flip" default:NO];
}
- (void)draw {
    [self fillBackgroundIfNeeded];
    double rel = RMRelativeValue(self.measures.firstObject);
    NSRect f = self.frame;
    [_barColor setFill];
    if ([_orientation hasPrefix:@"horiz"]) {
        CGFloat bw = f.size.width * rel;
        NSRect bar = _flip ? NSMakeRect(f.origin.x + f.size.width - bw, f.origin.y, bw, f.size.height)
                           : NSMakeRect(f.origin.x, f.origin.y, bw, f.size.height);
        NSRectFillUsingOperation(bar, NSCompositingOperationSourceOver);
    } else {
        CGFloat bh = f.size.height * rel;
        // Flipped view: y grows downward. Default bar grows from bottom up.
        NSRect bar = _flip ? NSMakeRect(f.origin.x, f.origin.y, f.size.width, bh)
                           : NSMakeRect(f.origin.x, f.origin.y + f.size.height - bh, f.size.width, bh);
        NSRectFillUsingOperation(bar, NSCompositingOperationSourceOver);
    }
}
@end

#pragma mark - Line

@implementation RMMeterLine {
    NSColor *_lineColor;
    NSMutableArray<NSNumber *> *_history;
    double _scaleV;
}
- (void)readSubclassOptions {
    _lineColor = RMParseColor([self.parser readString:self.name key:@"LineColor" default:nil],
                              [NSColor greenColor]);
    _scaleV = [self.parser readDouble:self.name key:@"LineScale" default:1];
    _history = [NSMutableArray array];
}
- (void)prepare {
    RMMeasure *m = self.measures.firstObject;
    [_history addObject:@(RMRelativeValue(m))];
    NSInteger cap = (NSInteger)MAX(self.w, 1);
    while ((NSInteger)_history.count > cap) [_history removeObjectAtIndex:0];
}
- (void)draw {
    [self fillBackgroundIfNeeded];
    if (_history.count < 2) return;
    NSRect f = self.frame;
    NSBezierPath *path = [NSBezierPath bezierPath];
    NSInteger n = _history.count;
    for (NSInteger i = 0; i < n; i++) {
        CGFloat px = f.origin.x + f.size.width * (CGFloat)i / (CGFloat)MAX(n - 1, 1);
        CGFloat rel = _history[i].doubleValue;
        CGFloat py = f.origin.y + f.size.height - f.size.height * rel;  // flipped
        if (i == 0) [path moveToPoint:NSMakePoint(px, py)];
        else [path lineToPoint:NSMakePoint(px, py)];
    }
    [_lineColor setStroke];
    path.lineWidth = 1.0;
    [path stroke];
}
@end

#pragma mark - Histogram

@implementation RMMeterHistogram {
    NSColor *_primary;
    NSMutableArray<NSNumber *> *_history;
}
- (void)readSubclassOptions {
    _primary = RMParseColor([self.parser readString:self.name key:@"PrimaryColor" default:nil],
                            [NSColor greenColor]);
    _history = [NSMutableArray array];
}
- (void)prepare {
    [_history addObject:@(RMRelativeValue(self.measures.firstObject))];
    NSInteger cap = (NSInteger)MAX(self.w, 1);
    while ((NSInteger)_history.count > cap) [_history removeObjectAtIndex:0];
}
- (void)draw {
    [self fillBackgroundIfNeeded];
    NSRect f = self.frame;
    [_primary setFill];
    NSInteger n = _history.count;
    for (NSInteger i = 0; i < n; i++) {
        CGFloat rel = _history[i].doubleValue;
        CGFloat bh = f.size.height * rel;
        NSRect bar = NSMakeRect(f.origin.x + i, f.origin.y + f.size.height - bh, 1, bh);
        NSRectFillUsingOperation(bar, NSCompositingOperationSourceOver);
    }
}
@end

#pragma mark - Rotator

@implementation RMMeterRotator {
    NSString *_imagePath; NSImage *_image;
    double _startAngle, _rotationAngle;
    CGFloat _offsetX, _offsetY;
}
- (void)readSubclassOptions {
    _imagePath = [self.parser readString:self.name key:@"ImageName" default:nil];
    _startAngle = [self.parser readDouble:self.name key:@"StartAngle" default:0];
    _rotationAngle = [self.parser readDouble:self.name key:@"RotationAngle" default:6.2832];
    _offsetX = [self.parser readDouble:self.name key:@"OffsetX" default:0];
    _offsetY = [self.parser readDouble:self.name key:@"OffsetY" default:0];
    if (_imagePath.length) {
        NSString *resolved = RMResolveImagePath(_imagePath, self.parser);
        _image = [[NSImage alloc] initWithContentsOfFile:resolved];
    }
}
- (void)draw {
    [self fillBackgroundIfNeeded];
    if (_image == nil) return;
    double rel = RMRelativeValue(self.measures.firstObject);
    double angle = _startAngle + _rotationAngle * rel;
    NSRect f = self.frame;
    NSGraphicsContext *ctx = [NSGraphicsContext currentContext];
    [ctx saveGraphicsState];
    NSAffineTransform *tr = [NSAffineTransform transform];
    CGFloat cx = f.origin.x + f.size.width / 2.0;
    CGFloat cy = f.origin.y + f.size.height / 2.0;
    [tr translateXBy:cx yBy:cy];
    [tr rotateByRadians:-angle];  // flipped view → negate for clockwise
    [tr concat];
    NSRect dst = NSMakeRect(-_offsetX, -_offsetY, _image.size.width, _image.size.height);
    [_image drawInRect:dst fromRect:NSZeroRect
             operation:NSCompositingOperationSourceOver fraction:1.0 respectFlipped:YES hints:nil];
    [ctx restoreGraphicsState];
}
@end

#pragma mark - RoundLine

@implementation RMMeterRoundLine {
    NSColor *_lineColor;
    double _lineLength, _lineStart, _startAngle, _rotationAngle;
    BOOL _solid;
}
- (void)readSubclassOptions {
    _lineColor = RMParseColor([self.parser readString:self.name key:@"LineColor" default:nil],
                              [NSColor whiteColor]);
    _lineLength = [self.parser readDouble:self.name key:@"LineLength" default:10];
    _lineStart = [self.parser readDouble:self.name key:@"LineStart" default:0];
    _startAngle = [self.parser readDouble:self.name key:@"StartAngle" default:0];
    _rotationAngle = [self.parser readDouble:self.name key:@"RotationAngle" default:6.2832];
    _solid = [self.parser readBool:self.name key:@"Solid" default:NO];
}
- (void)draw {
    [self fillBackgroundIfNeeded];
    double rel = RMRelativeValue(self.measures.firstObject);
    NSRect f = self.frame;
    CGFloat cx = f.origin.x + f.size.width / 2.0;
    CGFloat cy = f.origin.y + f.size.height / 2.0;
    double angle = _startAngle + _rotationAngle * rel;
    if (_solid) {
        NSBezierPath *p = [NSBezierPath bezierPath];
        [p moveToPoint:NSMakePoint(cx, cy)];
        [p appendBezierPathWithArcWithCenter:NSMakePoint(cx, cy)
                                      radius:_lineLength
                                  startAngle:-_startAngle * 180.0 / M_PI
                                    endAngle:-angle * 180.0 / M_PI
                                   clockwise:YES];
        [p closePath];
        [_lineColor setFill];
        [p fill];
    } else {
        CGFloat ex = cx + cos(angle) * _lineLength;
        CGFloat ey = cy + sin(angle) * _lineLength;
        CGFloat sx = cx + cos(angle) * _lineStart;
        CGFloat sy = cy + sin(angle) * _lineStart;
        NSBezierPath *p = [NSBezierPath bezierPath];
        [p moveToPoint:NSMakePoint(sx, sy)];
        [p lineToPoint:NSMakePoint(ex, ey)];
        [_lineColor setStroke];
        p.lineWidth = 1.0;
        [p stroke];
    }
}
@end

#pragma mark - Shape (rectangle / rounded rect / ellipse / line + Combine)

// One parsed shape primitive with its paint attributes.
@interface RMShapeDef : NSObject
@property (nonatomic, strong, nullable) NSBezierPath *path;   // nil for Combine
@property (nonatomic, strong, nullable) NSColor *fill;
@property (nonatomic, strong, nullable) NSColor *stroke;
@property (nonatomic, assign) CGFloat strokeWidth;
@property (nonatomic, assign) NSPoint offset;
@property (nonatomic, assign) BOOL isCombine;
@property (nonatomic, copy, nullable) NSString *combineBase;         // e.g. "Shape2"
@property (nonatomic, strong) NSMutableArray<NSArray *> *combineOps; // [op, name]
@property (nonatomic, assign) BOOL consumed;                          // merged into a Combine
@end
@implementation RMShapeDef
- (instancetype)init { if ((self = [super init])) { _combineOps = [NSMutableArray array]; _strokeWidth = 0; } return self; }
@end

@implementation RMMeterShape

// Build an NSBezierPath for a primitive token like "Rectangle 0,0,100,80,8".
// Coordinates are relative to the meter's X/Y (added by the caller via offset).
static NSBezierPath *RMBuildPrimitive(NSString *head, RMConfigParser *cp) {
    NSString *trimmed = [head stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSRange sp = [trimmed rangeOfString:@" "];
    if (sp.location == NSNotFound) return nil;
    NSString *kind = [trimmed substringToIndex:sp.location].lowercaseString;
    NSString *argStr = [trimmed substringFromIndex:sp.location + 1];
    NSArray<NSString *> *a = [argStr componentsSeparatedByString:@","];
    double (^num)(NSUInteger) = ^double(NSUInteger i) {
        return i < a.count ? [RMConfigParser evaluateNumber:a[i] default:0] : 0;
    };
    if ([kind isEqualToString:@"rectangle"]) {
        if (a.count < 4) return nil;
        NSRect r = NSMakeRect(num(0), num(1), num(2), num(3));
        if (a.count >= 5) {
            double rad = num(4);
            return [NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad];
        }
        return [NSBezierPath bezierPathWithRect:r];
    }
    if ([kind isEqualToString:@"roundedrectangle"] || [kind isEqualToString:@"roundedrect"]) {
        if (a.count < 4) return nil;
        NSRect r = NSMakeRect(num(0), num(1), num(2), num(3));
        double rad = a.count >= 5 ? num(4) : 0;
        return [NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad];
    }
    if ([kind isEqualToString:@"ellipse"]) {
        if (a.count < 3) return nil;
        double cx = num(0), cy = num(1), rx = num(2), ry = a.count >= 4 ? num(3) : rx;
        return [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - rx, cy - ry, rx * 2, ry * 2)];
    }
    if ([kind isEqualToString:@"line"]) {
        if (a.count < 4) return nil;
        NSBezierPath *p = [NSBezierPath bezierPath];
        [p moveToPoint:NSMakePoint(num(0), num(1))];
        [p lineToPoint:NSMakePoint(num(2), num(3))];
        return p;
    }
    if ([kind isEqualToString:@"arc"]) {
        // Arc CX,CY,RX,RY,StartAngle,SweepAngle
        if (a.count < 6) return nil;
        double cx = num(0), cy = num(1), rx = num(2), ry = num(3);
        double startDeg = num(4), sweepDeg = num(5);
        double startRad = startDeg * M_PI / 180.0;
        double endRad   = (startDeg + sweepDeg) * M_PI / 180.0;
        // NSBezierPath arc is clockwise; Rainmeter's positive sweep is clockwise.
        // In the flipped view, we negate Y for correct orientation.
        NSBezierPath *p = [NSBezierPath bezierPath];
        if (fabs(rx - ry) < 0.01) {
            [p appendBezierPathWithArcWithCenter:NSMakePoint(cx, cy) radius:rx
                                      startAngle:-startRad endAngle:-endRad clockwise:YES];
        } else {
            // Elliptical arc approximation using transform.
            [p appendBezierPathWithArcWithCenter:NSMakePoint(cx, cy) radius:1.0
                                      startAngle:-startRad endAngle:-endRad clockwise:YES];
            NSAffineTransform *st = [NSAffineTransform transform];
            [st translateXBy:cx yBy:cy];
            [st scaleXBy:rx yBy:ry];
            [st translateXBy:-cx yBy:-cy];
            [p transformUsingAffineTransform:st];
        }
        return p;
    }
    if ([kind isEqualToString:@"curve"]) {
        // Curve X1,Y1,CX1,CY1,CX2,CY2,X2,Y2 (cubic bezier)
        if (a.count < 8) return nil;
        NSBezierPath *p = [NSBezierPath bezierPath];
        [p moveToPoint:NSMakePoint(num(0), num(1))];
        [p curveToPoint:NSMakePoint(num(6), num(7))
          controlPoint1:NSMakePoint(num(2), num(3))
          controlPoint2:NSMakePoint(num(4), num(5))];
        return p;
    }
    if ([kind isEqualToString:@"path"]) {
        // Path X1,Y1 | LineTo X2,Y2 | ArcTo CX,CY,RX,RY,Start,Sweep | ClosePath 1
        // Simplified path parser: moveTo → series of commands separated by |
        // We accept: LineTo, ArcTo, CurveTo, ClosePath
        NSString *restStr = [trimmed substringFromIndex:sp.location + 1];
        NSArray *pathParts = [restStr componentsSeparatedByString:@"|"];
        NSBezierPath *p = [NSBezierPath bezierPath];
        BOOL started = NO;
        for (NSString *cmd in pathParts) {
            NSString *cmdTrim = [cmd stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            NSString *cmdLow = cmdTrim.lowercaseString;
            if ([cmdLow hasPrefix:@"lineto"]) {
                NSArray *n = [[cmdTrim substringFromIndex:6] componentsSeparatedByString:@","];
                if (n.count >= 2) {
                    double x = [n[0] doubleValue], y = [n[1] doubleValue];
                    if (!started) { [p moveToPoint:NSMakePoint(x, y)]; started = YES; }
                    else [p lineToPoint:NSMakePoint(x, y)];
                }
            } else if ([cmdLow hasPrefix:@"arcto"]) {
                NSArray *n = [[cmdTrim substringFromIndex:5] componentsSeparatedByString:@","];
                if (n.count >= 6 && started) {
                    double cx = [n[0] doubleValue], cy = [n[1] doubleValue];
                    double rx = [n[2] doubleValue], ry = [n[3] doubleValue];
                    double sDeg = [n[4] doubleValue], swDeg = [n[5] doubleValue];
                    double sRad = sDeg * M_PI / 180.0, eRad = (sDeg + swDeg) * M_PI / 180.0;
                    [p appendBezierPathWithArcWithCenter:NSMakePoint(cx, cy) radius:rx
                                              startAngle:-sRad endAngle:-eRad clockwise:YES];
                }
            } else if ([cmdLow hasPrefix:@"curveto"]) {
                NSArray *n = [[cmdTrim substringFromIndex:7] componentsSeparatedByString:@","];
                if (n.count >= 6 && started) {
                    [p curveToPoint:NSMakePoint([n[4] doubleValue], [n[5] doubleValue])
                      controlPoint1:NSMakePoint([n[0] doubleValue], [n[1] doubleValue])
                      controlPoint2:NSMakePoint([n[2] doubleValue], [n[3] doubleValue])];
                }
            } else if ([cmdLow hasPrefix:@"closepath"]) {
                if (started) [p closePath];
            } else {
                // Assume it's a coordinate pair: X,Y
                NSArray *n = [cmdTrim componentsSeparatedByString:@","];
                if (n.count >= 2) {
                    double x = [n[0] doubleValue], y = [n[1] doubleValue];
                    if (!started) { [p moveToPoint:NSMakePoint(x, y)]; started = YES; }
                    else [p lineToPoint:NSMakePoint(x, y)];
                }
            }
        }
        return started ? p : nil;
    }
    return nil;
}

- (RMShapeDef *)parseShape:(NSString *)raw {
    RMConfigParser *cp = self.parser;
    NSArray<NSString *> *parts = [raw componentsSeparatedByString:@"|"];
    if (parts.count == 0) return nil;
    RMShapeDef *def = [RMShapeDef new];
    NSString *head = [parts[0] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];

    if ([head.lowercaseString hasPrefix:@"combine"]) {
        def.isCombine = YES;
        NSString *rest = [[head substringFromIndex:7]
                          stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        def.combineBase = rest;
    } else {
        def.path = RMBuildPrimitive(head, cp);
        if (def.path == nil && !def.isCombine) return nil;
    }

    for (NSUInteger i = 1; i < parts.count; i++) {
        NSString *mod = [parts[i] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSString *low = mod.lowercaseString;
        if ([low hasPrefix:@"fill color"]) {
            def.fill = RMParseColor([mod substringFromIndex:10], def.fill);
        } else if ([low hasPrefix:@"fillcolor"]) {
            def.fill = RMParseColor([mod substringFromIndex:9], def.fill);
        } else if ([low hasPrefix:@"stroke color"]) {
            def.stroke = RMParseColor([mod substringFromIndex:12], def.stroke);
        } else if ([low hasPrefix:@"strokecolor"]) {
            def.stroke = RMParseColor([mod substringFromIndex:11], def.stroke);
        } else if ([low hasPrefix:@"strokewidth"]) {
            def.strokeWidth = [RMConfigParser evaluateNumber:[mod substringFromIndex:11] default:0];
        } else if ([low hasPrefix:@"offset"]) {
            NSArray *o = [[mod substringFromIndex:6] componentsSeparatedByString:@","];
            if (o.count >= 2) def.offset = NSMakePoint([RMConfigParser evaluateNumber:o[0] default:0],
                                                       [RMConfigParser evaluateNumber:o[1] default:0]);
        } else if ([low hasPrefix:@"union"] || [low hasPrefix:@"exclude"] ||
                   [low hasPrefix:@"intersect"] || [low hasPrefix:@"xor"]) {
            NSRange s = [mod rangeOfString:@" "];
            if (s.location != NSNotFound) {
                NSString *op = [mod substringToIndex:s.location].lowercaseString;
                NSString *nm = [[mod substringFromIndex:s.location + 1]
                                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                [def.combineOps addObject:@[op, nm]];
            }
        }
        // Rotate / Scale / Skew / StrokeStartCap etc. are ignored for now.
    }
    return def;
}

- (void)prepare {
    // Compute the union bounds of all shapes so the meter contributes to the
    // skin content size (otherwise rounded-rect backgrounds get clipped away).
    RMConfigParser *cp = self.parser;
    RMIniSection *sec = [cp.ini sectionNamed:self.name];
    if (sec == nil) return;
    NSRect bounds = NSZeroRect;
    BOOL any = NO;
    for (int i = 1; i <= 64; i++) {
        NSString *key = (i == 1) ? @"Shape" : [NSString stringWithFormat:@"Shape%d", i];
        NSString *rawV = [sec valueForKey:key];
        if (rawV.length == 0) { if (i > 1) break; else continue; }
        RMShapeDef *d = [self parseShape:[cp expand:rawV]];
        if (d == nil || d.path == nil || d.path.isEmpty) continue;
        NSRect b = NSOffsetRect(d.path.bounds, d.offset.x, d.offset.y);
        bounds = any ? NSUnionRect(bounds, b) : b;
        any = YES;
    }
    if (any) {
        if (self.w <= 0) self.w = ceil(NSMaxX(bounds));
        if (self.h <= 0) self.h = ceil(NSMaxY(bounds));
    }
}

- (void)draw {
    [self fillBackgroundIfNeeded];
    RMConfigParser *cp = self.parser;
    RMIniSection *sec = [cp.ini sectionNamed:self.name];
    if (sec == nil) return;
    // Collect Shape, Shape2 ... in order and parse each (expanded per draw so
    // DynamicVariables shapes update). Store under their key for Combine refs.
    NSMutableArray<NSString *> *keys = [NSMutableArray array];
    NSMutableArray<RMShapeDef *> *defs = [NSMutableArray array];
    NSMutableDictionary<NSString *, RMShapeDef *> *byKey = [NSMutableDictionary dictionary];
    for (int i = 1; i <= 64; i++) {
        NSString *key = (i == 1) ? @"Shape" : [NSString stringWithFormat:@"Shape%d", i];
        NSString *rawV = [sec valueForKey:key];
        if (rawV.length == 0) { if (i > 1) break; else continue; }
        NSString *expanded = [cp expand:rawV];
        RMShapeDef *d = [self parseShape:expanded];
        if (d == nil) continue;
        [keys addObject:key];
        [defs addObject:d];
        byKey[key.uppercaseString] = d;
    }
    if (defs.count == 0) return;

    // Resolve Combine shapes: build merged paths and mark sources as consumed.
    for (RMShapeDef *d in defs) {
        if (!d.isCombine) continue;
        RMShapeDef *base = byKey[d.combineBase.uppercaseString];
        if (base == nil || base.path == nil) continue;
        NSBezierPath *merged = [base.path copy];
        BOOL anyExclude = NO;
        base.consumed = YES;
        for (NSArray *opPair in d.combineOps) {
            NSString *op = opPair[0];
            RMShapeDef *other = byKey[[opPair[1] uppercaseString]];
            if (other == nil || other.path == nil) continue;
            other.consumed = YES;
            [merged appendBezierPath:other.path];
            if ([op isEqualToString:@"exclude"] || [op isEqualToString:@"xor"]) anyExclude = YES;
        }
        // Even-odd winding turns a fully-enclosed sub-shape into a hole, which
        // approximates Exclude/XOR (e.g. an outer rect minus an inner rect =
        // border ring). Union just accumulates with non-zero winding.
        merged.windingRule = anyExclude ? NSWindingRuleEvenOdd : NSWindingRuleNonZero;
        d.path = merged;
        // Inherit paint from the base shape when the combine has none of its own.
        if (d.fill == nil) d.fill = base.fill;
        if (d.stroke == nil) d.stroke = base.stroke;
        if (d.strokeWidth == 0) d.strokeWidth = base.strokeWidth;
    }

    NSGraphicsContext *gc = [NSGraphicsContext currentContext];
    for (NSUInteger i = 0; i < defs.count; i++) {
        RMShapeDef *d = defs[i];
        if (d.consumed || d.path == nil) continue;
        [gc saveGraphicsState];
        NSAffineTransform *tr = [NSAffineTransform transform];
        [tr translateXBy:self.x + d.offset.x yBy:self.y + d.offset.y];
        [tr concat];
        // Rainmeter's default fill is *transparent* — only paint if the skin
        // actually specified a colour. Defaulting to opaque black (what we did
        // before) turned every unstyled background into a solid black box.
        if (d.fill) {
            [d.fill setFill];
            [d.path fill];
        }
        if (d.strokeWidth > 0) {
            [(d.stroke ?: [NSColor blackColor]) setStroke];
            d.path.lineWidth = d.strokeWidth;
            [d.path stroke];
        }
        [gc restoreGraphicsState];
    }
}
@end

#pragma mark - Button

// Button meter: renders a bitmap image strip (like Bitmap) but responds to
// mouse-down by switching to a "pressed" frame. Supports LeftMouseDownAction
// in addition to LeftMouseUpAction. The button image is a vertical strip of
// frames: [normal, hover, pressed].
@implementation RMMeterButton {
    NSString *_buttonImagePath;
    NSImage *_buttonImage;
    NSString *_loadedPath;
    NSColor *_tint;
    int _frameCount;
    CGFloat _frameH;
    BOOL _pressed;
}
- (void)readSubclassOptions {
    _buttonImagePath = [self.parser readString:self.name key:@"ButtonImage" default:nil];
    _tint = RMParseColor([self.parser readString:self.name key:@"ImageTint" default:nil], nil);
    _frameCount = [self.parser readInt:self.name key:@"ButtonFrameCount" default:3];
    if (_frameCount < 1) _frameCount = 3;
    _loadedPath = nil;
    _pressed = NO;
}
- (void)loadIfNeeded {
    if (_buttonImagePath.length == 0) return;
    NSString *resolved = RMResolveImagePath(_buttonImagePath, self.parser);
    if ([_loadedPath isEqualToString:resolved] && _buttonImage) return;
    _loadedPath = resolved;
    _buttonImage = [[NSImage alloc] initWithContentsOfFile:resolved];
    if (_buttonImage == nil) { RMLogDebug(@"button image not found: %@", resolved); return; }
    if (_tint) _buttonImage = RMTintImage(_buttonImage, _tint);
    _frameH = _buttonImage.size.height / _frameCount;
}
- (void)prepare {
    [self loadIfNeeded];
    if (_buttonImage) {
        if (self.w <= 0) self.w = _buttonImage.size.width;
        if (self.h <= 0) self.h = _frameH;
    }
}
// Frame 0 = normal, 1 = hover, 2 = pressed
- (int)currentFrame {
    if (_pressed) return MIN(2, _frameCount - 1);
    if (self.hovered && _frameCount >= 2) return 1;
    return 0;
}
- (void)draw {
    [self fillBackgroundIfNeeded];
    if (_buttonImage == nil || _frameCount < 1) return;
    int frame = [self currentFrame];
    CGFloat srcY = _buttonImage.size.height - (frame + 1) * _frameH;
    NSRect dst = NSMakeRect(self.x, self.y, self.w > 0 ? self.w : _buttonImage.size.width, self.h > 0 ? self.h : _frameH);
    NSRect src = NSMakeRect(0, srcY, _buttonImage.size.width, _frameH);
    [_buttonImage drawInRect:dst fromRect:src operation:NSCompositingOperationSourceOver
                   fraction:1.0 respectFlipped:YES hints:nil];
}
@end

#pragma mark - Bitmap (digit-strip font)

@implementation RMMeterBitmap {
    NSString *_imagePath;
    NSImage *_image;
    NSString *_loadedPath;
    NSColor *_tint;
    int _frameCount;
    int _digits;
    int _separation;
    BOOL _extend;
    BOOL _zeroFrame;
    NSString *_align;
    CGFloat _frameW, _frameH;
    BOOL _vertical;
}
- (void)readSubclassOptions {
    _imagePath = [self.parser readString:self.name key:@"BitmapImage" default:nil];
    _tint = RMParseColor([self.parser readString:self.name key:@"ImageTint" default:nil], nil);
    _frameCount = [self.parser readInt:self.name key:@"BitmapFrames" default:1];
    _digits = [self.parser readInt:self.name key:@"BitmapDigits" default:0];
    _separation = [self.parser readInt:self.name key:@"BitmapSeparation" default:0];
    _extend = [self.parser readBool:self.name key:@"BitmapExtend" default:NO];
    _zeroFrame = [self.parser readBool:self.name key:@"BitmapZeroFrame" default:NO];
    _align = ([self.parser readString:self.name key:@"BitmapAlign" default:@"LEFT"]).lowercaseString;
    _loadedPath = nil;
}
- (void)loadIfNeeded {
    if (_imagePath.length == 0) return;
    NSString *resolved = RMResolveImagePath(_imagePath, self.parser);
    if ([_loadedPath isEqualToString:resolved] && _image) return;
    _loadedPath = resolved;
    _image = [[NSImage alloc] initWithContentsOfFile:resolved];
    if (_image == nil) { RMLogDebug(@"bitmap image not found: %@", resolved); return; }
    if (_tint) _image = RMTintImage(_image, _tint);

    NSSize sz = _image.size;
    _vertical = sz.height > sz.width;
    if (_frameCount < 1) _frameCount = 1;
    if (_vertical) { _frameW = sz.width; _frameH = sz.height / _frameCount; }
    else           { _frameW = sz.width / _frameCount; _frameH = sz.height; }
}
- (void)prepare {
    [self loadIfNeeded];
    if (_image) {
        int digits = MAX(1, _digits);
        if (self.w <= 0) self.w = _extend ? (_frameW * digits + (digits - 1) * _separation) : _frameW;
        if (self.h <= 0) self.h = _frameH;
    }
}
// Draw one frame index `frame` at the given top-left rect origin.
- (void)drawFrame:(int)frame atX:(CGFloat)dx y:(CGFloat)dy {
    NSSize sz = _image.size;
    CGFloat srcX, srcY;
    if (_vertical) {
        srcX = 0;
        // Frame 0 is at the top of the file; NSImage uses a bottom-left origin.
        srcY = sz.height - (frame + 1) * _frameH;
    } else {
        srcX = frame * _frameW;
        srcY = 0;
    }
    NSRect dst = NSMakeRect(dx, dy, _frameW, _frameH);
    NSRect src = NSMakeRect(srcX, srcY, _frameW, _frameH);
    [_image drawInRect:dst fromRect:src operation:NSCompositingOperationSourceOver
              fraction:1.0 respectFlipped:YES hints:nil];
}
- (void)draw {
    [self fillBackgroundIfNeeded];
    if (_image == nil || _frameCount < 1) return;
    RMMeasure *m = self.measures.firstObject;

    if (_extend) {
        long long value = (long long)(m ? m.value : 0);
        if (value < 0) value = 0;
        int numOfNums = 0;
        if (_digits > 0) {
            numOfNums = _digits;
        } else {
            long long tmp = value;
            do { ++numOfNums; tmp = (_frameCount == 1) ? tmp / 2 : tmp / _frameCount; } while (tmp > 0);
        }
        CGFloat step = _frameW + _separation;
        CGFloat offset;
        if ([_align isEqualToString:@"right"])       offset = 0;
        else if ([_align isEqualToString:@"center"]) offset = numOfNums * step / 2.0;
        else                                          offset = numOfNums * step;   // left
        int remaining = numOfNums;
        do {
            offset -= step;
            int frame = (int)(value % _frameCount);
            [self drawFrame:frame atX:self.x + offset y:self.y];
            value = (_frameCount == 1) ? value / 2 : value / _frameCount;
            --remaining;
        } while (remaining > 0);
    } else {
        double rel = RMRelativeValue(m);
        int frame;
        if (_zeroFrame) frame = (m && m.value > 0.0) ? (int)(rel * (_frameCount - 1)) : 0;
        else            frame = (int)(rel * _frameCount);
        if (frame >= _frameCount) frame = _frameCount - 1;
        if (frame < 0) frame = 0;
        [self drawFrame:frame atX:self.x y:self.y];
    }
}
@end
