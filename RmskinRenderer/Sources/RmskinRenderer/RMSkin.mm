#import "RMSkin.h"
#import "RMConfigParser.h"
#import "RMMeasure.h"
#import "RMMeter.h"
#import "RMBangs.h"
#import "RMFontManager.h"
#import "RMLog.h"

@implementation RMSkin {
    NSString *_skinFile;
    NSString *_resources;
    NSString *_rootConfig;
    NSString *_skinsPath;
    NSString *_config;

    NSMutableArray<RMMeasure *> *_measures;
    NSMutableArray<RMMeter *>   *_meters;
    NSMutableDictionary<NSString *, RMMeasure *> *_measureByName; // UPPER
    NSMutableDictionary<NSString *, RMMeter *>   *_meterByName;   // UPPER

    NSColor  *_bgColor;
    NSImage  *_bgImage;
    int       _backgroundMode;
    NSColor  *_blurTint;
    int       _defaultUpdateDivider;

    // Reentrancy guards: a bang fired from within tick/reload (e.g. !Update,
    // !Refresh triggered by an IfAction) must not recursively re-enter the
    // same pipeline, which would otherwise overflow the stack.
    BOOL      _ticking;
    BOOL      _reloading;
    BOOL      _hasFiredRefreshAction;
}

- (nullable instancetype)initWithSkinFile:(NSString *)skinFile
                                resources:(NSString *)resources
                               rootConfig:(NSString *)rootConfig
                                skinsPath:(NSString *)skinsPath
                                   config:(NSString *)config {
    if ((self = [super init])) {
        _skinFile = skinFile;
        _resources = resources;
        _rootConfig = rootConfig;
        _skinsPath = skinsPath;
        _config = config;
        [[RMFontManager shared] registerFontsInDirectory:
            [resources stringByAppendingPathComponent:@"Fonts"]];
        [[RMFontManager shared] registerFontsInDirectory:resources];
        if (![self reload]) return nil;
    }
    return self;
}

#pragma mark - Build

- (BOOL)reload {
    // Guard against a bang (e.g. !Refresh from an IfAction) re-entering reload
    // while a reload is already in progress.
    if (_reloading) return YES;
    _reloading = YES;
    _parser = [RMConfigParser new];
    _parser.resourcesPath = [_resources hasSuffix:@"/"] ? _resources
                                                        : [_resources stringByAppendingString:@"/"];
    _parser.rootConfigPath = _rootConfig;
    _parser.skinsPath = _skinsPath;
    _parser.currentPath = [_skinFile stringByDeletingLastPathComponent];
    _parser.currentConfig = _config;

    __weak RMSkin *weakSelf = self;
    _parser.sectionVariableResolver = ^NSString *(NSString *token) {
        return [weakSelf resolveSectionVariable:token];
    };
    _parser.measureValueResolver = ^BOOL(NSString *name, double *out) {
        return [weakSelf resolveMeasureValue:name into:out];
    };
    _parser.measureStringResolver = ^NSString *(NSString *name) {
        RMSkin *strongSelf = weakSelf;
        if (strongSelf == nil) return nil;
        RMMeasure *m = strongSelf->_measureByName[name.uppercaseString];
        if (m == nil) return nil;
        NSString *val = [m rawString] ?: @"";
        return [m applySubstitute:val];
    };

    if (![_parser loadSkinFile:_skinFile]) {
        RMLogError(@"failed to load skin: %@", _skinFile);
        _reloading = NO;
        return NO;
    }

    _measures = [NSMutableArray array];
    _meters = [NSMutableArray array];
    _measureByName = [NSMutableDictionary dictionary];
    _meterByName = [NSMutableDictionary dictionary];

    [self readRainmeterSection];

    // --- Pass 1: construct all measures and meters, register in lookup tables
    // so that [MeasureName:Selector] / [MeterName:X] section variables work
    // regardless of definition order. -------------------------------------------------
    NSMutableArray<RMMeasure *> *pendingMeasures = [NSMutableArray array];
    NSMutableArray<RMMeter *>   *pendingMeters   = [NSMutableArray array];
    BOOL hasFrostedGlass = NO;

    for (RMIniSection *section in _parser.ini.sections) {
        NSString *nameUpper = section.name.uppercaseString;
        if ([nameUpper isEqualToString:@"RAINMETER"] ||
            [nameUpper isEqualToString:@"METADATA"] ||
            [nameUpper isEqualToString:@"VARIABLES"]) continue;

        NSString *measureType = [section valueForKey:@"Measure"];
        NSString *meterType   = [section valueForKey:@"Meter"];

        if (measureType.length) {
            // FrostedGlass plugin → enable desktop blur backdrop on the view.
            NSString *plugin = [section valueForKey:@"Plugin"];
            if (plugin.length && [plugin.lowercaseString isEqualToString:@"frostedglass"]) {
                hasFrostedGlass = YES;
            }
            RMMeasure *m = [RMMeasure measureWithType:measureType name:section.name parser:_parser];
            if (m) {
                _measureByName[nameUpper] = m;
                [_measures addObject:m];
                __weak RMSkin *weakSelf = self;
                m.executeAction = ^(NSString *bang) { [weakSelf executeActions:bang]; };
                [pendingMeasures addObject:m];
            }
        } else if (meterType.length) {
            RMMeter *mt = [RMMeter meterWithType:meterType name:section.name parser:_parser];
            if (mt) {
                _meterByName[nameUpper] = mt;
                [_meters addObject:mt];
                [pendingMeters addObject:mt];
            }
        }
    }
    _useBlur = _useBlur || hasFrostedGlass;

    // Pass 2: read options now that all sections are in the lookup tables.
    // MeterStyle inheritance is applied just before reading the owning meter's
    // options so that derived keys (e.g. font size) are visible.
    for (RMMeter *mt in pendingMeters) {
        [self applyMeterStylesToSection:[_parser.ini sectionNamed:mt.name]];
        [mt readOptions];
    }
    for (RMMeasure *m in pendingMeasures) {
        m.defaultUpdateDivider = _defaultUpdateDivider;
        [m readOptions];
    }

    // Resolve meter → measure bindings.
    for (RMMeter *mt in _meters) {
        NSMutableArray<RMMeasure *> *bound = [NSMutableArray array];
        for (NSString *mn in mt.measureNames) {
            RMMeasure *ms = _measureByName[mn.uppercaseString];
            if (ms) [bound addObject:ms];
        }
        mt.measures = bound;
    }
    _reloading = NO;
    return YES;
}

// MeterStyle inheritance: a meter may name one or more style sections via
// "MeterStyle=StyleA | StyleB". Rainmeter treats each named style as a set of
// default options — the meter's own keys win, then StyleA, then StyleB, etc.
// We implement this by copying each style section's keys into the meter's own
// section with insert-if-absent semantics (so higher-priority keys are kept).
- (void)applyMeterStylesToSection:(RMIniSection *)section {
    NSString *raw = [section valueForKey:@"MeterStyle"];
    if (raw.length == 0) return;
    NSMutableArray<NSString *> *queue = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    // Collect style names in priority order, following nested MeterStyle links.
    void (^enqueue)(NSString *) = ^(NSString *spec) {
        for (NSString *part in [spec componentsSeparatedByString:@"|"]) {
            NSString *name = [part stringByTrimmingCharactersInSet:
                              [NSCharacterSet whitespaceCharacterSet]];
            if (name.length && ![seen containsObject:name.uppercaseString]) {
                [seen addObject:name.uppercaseString];
                [queue addObject:name];
            }
        }
    };
    enqueue(raw);
    for (NSUInteger i = 0; i < queue.count && i < 64; i++) {
        RMIniSection *style = [_parser.ini sectionNamed:queue[i]];
        if (style == nil) continue;
        NSString *nested = [style valueForKey:@"MeterStyle"];
        if (nested.length) enqueue(nested);
        for (NSString *key in style.orderedKeys) {
            if ([key.uppercaseString isEqualToString:@"METERSTYLE"]) continue;
            NSString *val = [style valueForKey:key];
            if (val) [section setValue:val forKey:key overwrite:NO];
        }
    }
}

- (void)readRainmeterSection {
    RMConfigParser *cp = _parser;
    int ms = [cp readInt:@"Rainmeter" key:@"Update" default:1000];
    _updateInterval = MAX(ms, 16) / 1000.0;
    _dynamicWindowSize = [cp readBool:@"Rainmeter" key:@"DynamicWindowSize" default:NO];
    _backgroundMode = [cp readInt:@"Rainmeter" key:@"BackgroundMode" default:0];
    _defaultUpdateDivider = [cp readInt:@"Rainmeter" key:@"DefaultUpdateDivider" default:1];
    if (_defaultUpdateDivider < 1) _defaultUpdateDivider = 1;

    // OnRefreshAction: fired once after skin loads/reloads.
    self.onRefreshAction = [cp readString:@"Rainmeter" key:@"OnRefreshAction" default:nil];
    // OnUpdateAction: fired every update tick (Rainmeter's OnUpdateAction).
    self.onUpdateAction  = [cp readString:@"Rainmeter" key:@"OnUpdateAction"  default:nil];

    // Blur=1 enables Rainmeter's built-in FrostedGlass-style backdrop.
    // We treat it the same as a FrostedGlass plugin measure: capture the
    // desktop behind the widget, blur, and composite a tint. BlurRegion
    // describes a rounded-rect shape (Type,X,Y,W,H,RadiusX,RadiusY) but
    // currently the visual effect uses the window's own frame.
    BOOL blur = [cp readBool:@"Rainmeter" key:@"Blur" default:NO];
    NSString *blurRegion = [cp readString:@"Rainmeter" key:@"BlurRegion" default:nil];
    if (blur || blurRegion.length > 0) {
        _useBlur = YES;
    }
    // Optional BlurTint in R,G,B,A form to override the default dark tint
    // for light themes (Quanto_Flx uses a white overlay).
    NSString *tintS = [cp readString:@"Rainmeter" key:@"BlurTint" default:nil];
    if (tintS.length) {
        NSArray *p = [tintS componentsSeparatedByString:@","];
        if (p.count >= 3) {
            CGFloat a = p.count >= 4 ? [p[3] doubleValue] / 255.0 : 0.5;
            _blurTint = [NSColor colorWithSRGBRed:[p[0] doubleValue]/255.0
                                            green:[p[1] doubleValue]/255.0
                                             blue:[p[2] doubleValue]/255.0 alpha:a];
        }
    }

    NSString *solid = [cp readString:@"Rainmeter" key:@"SolidColor" default:nil];
    _bgColor = nil;
    if (solid.length) {
        NSArray *p = [solid componentsSeparatedByString:@","];
        if (p.count >= 3) {
            CGFloat a = p.count >= 4 ? [p[3] doubleValue] / 255.0 : 1.0;
            _bgColor = [NSColor colorWithSRGBRed:[p[0] doubleValue]/255.0
                                           green:[p[1] doubleValue]/255.0
                                            blue:[p[2] doubleValue]/255.0 alpha:a];
        }
    }
    NSString *bg = [cp readString:@"Rainmeter" key:@"Background" default:nil];
    _bgImage = bg.length ? [[NSImage alloc] initWithContentsOfFile:bg] : nil;

    self.mouseScrollUpAction   = [cp readString:@"Rainmeter" key:@"MouseScrollUpAction"   default:nil];
    self.mouseScrollDownAction = [cp readString:@"Rainmeter" key:@"MouseScrollDownAction" default:nil];
}

#pragma mark - Section variables

- (nullable NSString *)resolveSectionVariable:(NSString *)token {
    // Rainmeter new-style section variable prefixes:
    //   [&MeasureName]  or  [&MeasureName:Selector]
    //   [#VariableName] or  [#VariableName:Selector]
    //   [$Config:Key]
    // Strip the prefix and dispatch accordingly.
    unichar prefix = 0;
    NSString *body = token;
    if (token.length > 0) {
        prefix = [token characterAtIndex:0];
        if (prefix == L'&' || prefix == L'#' || prefix == L'$') {
            body = [token substringFromIndex:1];
        } else {
            prefix = 0;   // old-style section variable
        }
    }

    if (prefix == L'#') {
        // [#VariableName] — look up in the variable table.
        NSString *val = [self.parser variableForName:body];
        if (val != nil) return val;
        // Fall through to old-style: [#MeasureName] also works.
    }
    if (prefix == L'$') {
        // [$ConfigName:Key] — cross-skin reference, not supported in this host.
        return nil;
    }

    NSRange colon = [body rangeOfString:@":"];
    NSString *base = colon.location == NSNotFound ? body : [body substringToIndex:colon.location];
    NSString *sel  = colon.location == NSNotFound ? nil : [body substringFromIndex:colon.location + 1];

    RMMeasure *m = _measureByName[base.uppercaseString];
    if (m) {
        if (sel.length == 0) return [m displayStringAutoScale:NO decimals:-1 percentual:NO scale:1];
        if ([sel isEqualToString:@"%"]) return [m displayStringAutoScale:NO decimals:0 percentual:YES scale:1];
        if ([sel caseInsensitiveCompare:@"MaxValue"] == NSOrderedSame) return [NSString stringWithFormat:@"%g", m.maxValue];
        if ([sel caseInsensitiveCompare:@"MinValue"] == NSOrderedSame) return [NSString stringWithFormat:@"%g", m.minValue];
        if ([sel hasPrefix:@"/"]) { double d = [[sel substringFromIndex:1] doubleValue]; return d != 0 ? [NSString stringWithFormat:@"%g", m.value/d] : @"0"; }
        int dec = [sel intValue];
        return [m displayStringAutoScale:NO decimals:dec percentual:NO scale:1];
    }

    RMMeter *mt = _meterByName[base.uppercaseString];
    if (mt) {
        NSString *s = sel.uppercaseString;
        if ([s isEqualToString:@"X"])  return [NSString stringWithFormat:@"%g", mt.x];
        if ([s isEqualToString:@"Y"])  return [NSString stringWithFormat:@"%g", mt.y];
        if ([s isEqualToString:@"W"])  return [NSString stringWithFormat:@"%g", mt.w];
        if ([s isEqualToString:@"H"])  return [NSString stringWithFormat:@"%g", mt.h];
        if ([s isEqualToString:@"XW"]) return [NSString stringWithFormat:@"%g", mt.x + mt.w];
        if ([s isEqualToString:@"YH"]) return [NSString stringWithFormat:@"%g", mt.y + mt.h];
    }
    return nil;
}

// Resolve a bare measure name used inside a Calc formula to its numeric value.
- (BOOL)resolveMeasureValue:(NSString *)name into:(double *)out {
    RMMeasure *m = _measureByName[name.uppercaseString];
    if (m == nil) return NO;
    if (out) *out = [m numericValue];
    return YES;
}

#pragma mark - Tick / size

- (void)tick {
    // Guard against a bang (e.g. !Update from an IfAction) re-entering tick
    // while a tick is already running — that path caused unbounded recursion.
    if (_ticking) return;
    _ticking = YES;

    // Fire OnRefreshAction on the first tick after reload.
    if (!_hasFiredRefreshAction) {
        _hasFiredRefreshAction = YES;
        if (self.onRefreshAction.length) {
            [self executeActions:self.onRefreshAction];
        }
    }

    for (RMMeasure *m in _measures) [m update];

    // OnUpdateAction fires every tick after measures are updated.
    if (self.onUpdateAction.length) {
        [self executeActions:self.onUpdateAction];
    }

    RMMeter *prev = nil;
    CGFloat maxX = 0, maxY = 0;
    for (RMMeter *mt in _meters) {
        [mt resetToAuthoredPosition];
        [mt resolvePositionWithPrevious:prev];
        [mt prepare];
        if (!mt.hidden) {
            maxX = MAX(maxX, mt.x + mt.w);
            maxY = MAX(maxY, mt.y + mt.h);
        }
        prev = mt;
    }
    _contentSize = NSMakeSize(ceil(maxX), ceil(maxY));
    if (_contentSize.width < 1) _contentSize.width = 1;
    if (_contentSize.height < 1) _contentSize.height = 1;
    _ticking = NO;
}

#pragma mark - Draw

- (void)drawInBounds:(NSRect)bounds {
    if (_bgImage) {
        // Rainmeter stretches the Background image to fill the entire skin
        // content area (determined by meter extents, not the image's natural
        // size). Draw it into the full bounds rectangle.
        [_bgImage drawInRect:bounds
                    fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                    fraction:1.0 respectFlipped:YES hints:nil];
    } else if (_bgColor && (_backgroundMode == 1 || _backgroundMode == 0)) {
        if (_bgColor.alphaComponent > 0.004) {
            [_bgColor setFill];
            NSRectFillUsingOperation(NSMakeRect(0, 0, _contentSize.width, _contentSize.height),
                                     NSCompositingOperationSourceOver);
        }
    }
    NSGraphicsContext *gc = [NSGraphicsContext currentContext];
    for (RMMeter *mt in _meters) {
        if (mt.hidden) continue;
        // Container semantics: a meter with Container=X is drawn with its
        // (x,y) shifted to the container's frame and clipped to it. Shift
        // self.x/self.y for the duration of draw, then restore, so the
        // meter's own draw code keeps treating self.x/self.y as its
        // "absolute" position.
        RMMeter *container = nil;
        CGFloat savedX = 0, savedY = 0;
        BOOL clipped = NO;
        if (mt.container.length) {
            container = _meterByName[mt.container.uppercaseString];
            if (container && container != mt) {
                savedX = mt.x; savedY = mt.y;
                mt.x = savedX + container.x;
                mt.y = savedY + container.y;
                if (container.w > 0 && container.h > 0) {
                    [gc saveGraphicsState];
                    NSRectClip(container.frame);
                    clipped = YES;
                }
            }
        }
        // TransformationMatrix support
        NSAffineTransform *matrix = [mt transformationMatrix];
        if (matrix) { [gc saveGraphicsState]; [matrix concat]; }
        @try { [mt draw]; }
        @catch (NSException *ex) { RMLogWarn(@"meter draw failed %@: %@", mt.name, ex); }
        if (matrix) { [gc restoreGraphicsState]; }
        if (clipped) {
            [gc restoreGraphicsState];
            mt.x = savedX;
            mt.y = savedY;
        }
    }
}

#pragma mark - Mouse

- (void)handleMouseUpAt:(NSPoint)point rightButton:(BOOL)rightButton {
    for (RMMeter *mt in _meters.reverseObjectEnumerator) {
        if (mt.hidden) continue;
        if (NSPointInRect(point, mt.frame)) {
            NSString *action = rightButton ? mt.rightMouseUpAction : mt.leftMouseUpAction;
            if (action.length) { [self executeActions:action]; return; }
        }
    }
}

- (void)handleScrollUp:(BOOL)up {
    NSString *a = up ? self.mouseScrollUpAction : self.mouseScrollDownAction;
    if (a.length) [self executeActions:a];
}

- (void)handleMouseMoveAt:(NSPoint)point {
    BOOL changed = NO;
    for (RMMeter *mt in _meters) {
        if (mt.mouseOverAction.length == 0 && mt.mouseLeaveAction.length == 0) continue;
        BOOL inside = !mt.hidden && NSPointInRect(point, mt.frame);
        if (inside && !mt.hovered) {
            mt.hovered = YES;
            if (mt.mouseOverAction.length) { [self executeActions:mt.mouseOverAction]; changed = YES; }
        } else if (!inside && mt.hovered) {
            mt.hovered = NO;
            if (mt.mouseLeaveAction.length) { [self executeActions:mt.mouseLeaveAction]; changed = YES; }
        }
    }
    if (changed) [self requestRedraw];
}

- (void)handleMouseExit {
    BOOL changed = NO;
    for (RMMeter *mt in _meters) {
        if (mt.hovered) {
            mt.hovered = NO;
            if (mt.mouseLeaveAction.length) { [self executeActions:mt.mouseLeaveAction]; changed = YES; }
        }
    }
    if (changed) [self requestRedraw];
}

#pragma mark - Bangs

- (void)executeActions:(NSString *)actions {
    [RMBangs execute:actions onSkin:self];
}

- (void)setOption:(NSString *)key value:(NSString *)value forSection:(NSString *)section {
    RMIniSection *s = [_parser.ini ensureSectionNamed:section];
    [s setValue:value forKey:key overwrite:YES];
    RMMeter *mt = _meterByName[section.uppercaseString];
    if (mt) { [mt readOptions]; [self rebindMeter:mt]; return; }
    RMMeasure *ms = _measureByName[section.uppercaseString];
    if (ms) { [ms readOptions]; }
}

- (void)rebindMeter:(RMMeter *)mt {
    NSMutableArray<RMMeasure *> *bound = [NSMutableArray array];
    for (NSString *mn in mt.measureNames) {
        RMMeasure *ms = _measureByName[mn.uppercaseString];
        if (ms) [bound addObject:ms];
    }
    mt.measures = bound;
}

- (void)requestRedraw { if (self.onNeedsRedraw) self.onNeedsRedraw(); }

#pragma mark - Lookups / accessors

- (NSArray<RMMeasure *> *)measures { return _measures; }
- (NSArray<RMMeter *> *)meters { return _meters; }
- (nullable RMMeter *)meterNamed:(NSString *)name { return _meterByName[name.uppercaseString]; }
- (nullable RMMeasure *)measureNamed:(NSString *)name { return _measureByName[name.uppercaseString]; }

// A meter/measure may declare several groups ("Group=A|B"); match any of them.
static BOOL RMBelongsToGroup(NSString *_Nullable groupSpec, NSString *query) {
    if (groupSpec.length == 0) return NO;
    for (NSString *g in [groupSpec componentsSeparatedByString:@"|"]) {
        NSString *t = [g stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([t caseInsensitiveCompare:query] == NSOrderedSame) return YES;
    }
    return NO;
}

- (NSArray<RMMeter *> *)metersInGroup:(NSString *)group {
    NSString *q = [group stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (q.length == 0) return @[];
    NSMutableArray<RMMeter *> *out = [NSMutableArray array];
    for (RMMeter *mt in _meters) if (RMBelongsToGroup(mt.group, q)) [out addObject:mt];
    return out;
}

- (NSArray<RMMeasure *> *)measuresInGroup:(NSString *)group {
    NSString *q = [group stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (q.length == 0) return @[];
    NSMutableArray<RMMeasure *> *out = [NSMutableArray array];
    for (RMMeasure *m in _measures) if (RMBelongsToGroup(m.group, q)) [out addObject:m];
    return out;
}

@end
