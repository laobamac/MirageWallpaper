#import "RMLayout.h"
#import "RMIniFile.h"
#import "RMConfigParser.h"
#import "RMLog.h"

@implementation RMLayoutEntry
@end

@implementation RMLayout {
    RMIniFile *_rmskin;
    RMConfigParser *_posParser; // for expanding #Var# in layout position values
}

- (nullable instancetype)initWithThemeDirectory:(NSString *)dir {
    if ((self = [super init])) {
        // Resolve to an absolute path so #@# / @Include expand correctly.
        if (!dir.isAbsolutePath) {
            dir = [NSURL fileURLWithPath:dir].path;
        }
        _themeDirectory = dir.copy;
        _skinsPath = [dir stringByAppendingPathComponent:@"Skins"];

        NSString *metaPath = [dir stringByAppendingPathComponent:@"RMSKIN.ini"];
        _rmskin = [RMIniFile new];
        if (![_rmskin parseContentsOfFile:metaPath]) {
            RMLogError(@"missing RMSKIN.ini: %@", metaPath);
            return nil;
        }
        RMIniSection *s = [_rmskin sectionNamed:@"rmskin"];
        _name     = [s valueForKey:@"Name"];
        _author   = [s valueForKey:@"Author"];
        _version  = [s valueForKey:@"Version"];
        _loadType = [s valueForKey:@"LoadType"];
        _load     = [s valueForKey:@"Load"];

        _posParser = [RMConfigParser new];
        _posParser.resourcesPath = @"";
        _posParser.rootConfigPath = @"";
        _posParser.skinsPath = _skinsPath;
        _posParser.currentPath = _themeDirectory;
        _posParser.currentConfig = @"";
    }
    return self;
}

- (NSArray<RMLayoutEntry *> *)activeEntries {
    NSString *lt = self.loadType.lowercaseString ?: @"";
    if ([lt isEqualToString:@"layout"]) return [self layoutEntries];
    return [self skinEntries];
}

#pragma mark - Layout

// Parse a Rainmeter position value: "300", "50%", "50.0%" — writes the number
// into *out and returns YES if the value ended with '%'.
static BOOL RMParsePositionValue(NSString *_Nullable s, CGFloat *out) {
    if (out) *out = 0;
    if (s.length == 0) return NO;
    NSString *v = [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    BOOL pct = NO;
    if ([v hasSuffix:@"%"]) { pct = YES; v = [v substringToIndex:v.length - 1]; }
    if (out) *out = [v doubleValue];
    if (pct && out) *out /= 100.0;   // store as fraction 0..1
    return pct;
}

// Returns YES when a raw position string contains Rainmeter formula syntax that
// our simple doubleValue parser cannot evaluate (e.g. "(#WORKAREAWIDTH#/2)",
// "(32*1.0+24)"). When the result is 0 and not a percentage, this indicates the
// position was not actually intended to be 0 — the formula just didn't resolve.
static BOOL RMPositionLooksUnresolved(NSString *_Nullable raw, CGFloat parsedVal, BOOL isPercent) {
    if (raw.length == 0) return NO;
    if (isPercent) return NO;  // percentage values go through fine (e.g. "50%")
    if (parsedVal != 0) return NO;  // a non-zero result means the formula evaluated
    NSString *v = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    // Heuristic: if the string contains formula characters and the parsed value
    // is 0, it is almost certainly an unresolved Rainmeter formula.
    NSRange r = [v rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"(#*+/-"]];
    return r.location != NSNotFound;
}

- (NSArray<RMLayoutEntry *> *)layoutEntries {
    NSArray<RMLayoutEntry *> *entries = [self layoutEntriesForLoad:self.load];

    // If the layout has very few entries (e.g. a _Setup wizard-only layout),
    // try the main layout (same name without _Setup suffix) as a fallback.
    if (entries.count <= 1 && [self.load hasSuffix:@"_Setup"]) {
        NSString *mainLoad = [self.load substringToIndex:self.load.length - 6];
        NSString *mainPath = [[[_themeDirectory stringByAppendingPathComponent:@"Layouts"]
                               stringByAppendingPathComponent:mainLoad]
                              stringByAppendingPathComponent:@"Rainmeter.ini"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:mainPath]) {
            RMLogDebug(@"setup layout has few entries; falling back to main layout: %@", mainLoad);
            NSArray<RMLayoutEntry *> *mainEntries = [self layoutEntriesForLoad:mainLoad];
            if (mainEntries.count > 0) return mainEntries;
        }
    }
    return entries;
}

- (NSString *)expandLayoutValue:(NSString *)raw {
    if (raw.length == 0) return raw;
    return [_posParser expand:raw];
}

- (NSArray<RMLayoutEntry *> *)layoutEntriesForLoad:(NSString *)loadName {
    NSString *layoutIni = [[[_themeDirectory stringByAppendingPathComponent:@"Layouts"]
                            stringByAppendingPathComponent:loadName ?: @""]
                           stringByAppendingPathComponent:@"Rainmeter.ini"];
    RMIniFile *ini = [RMIniFile new];
    if (![ini parseContentsOfFile:layoutIni]) {
        RMLogWarn(@"missing layout Rainmeter.ini: %@ — falling back to all skins", layoutIni);
        return [self allSkinEntries];
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray<RMLayoutEntry *> *entries = [NSMutableArray array];
    for (RMIniSection *sec in ini.sections) {
        if ([sec.name.uppercaseString isEqualToString:@"RAINMETER"]) continue;
        NSString *active = [sec valueForKey:@"Active"];
        if (active == nil || [active integerValue] < 1) continue;

        RMLayoutEntry *e = [self entryForConfig:sec.name variant:(int)[active integerValue] fm:fm];
        if (e == nil) continue;

        CGFloat wx, wy, ax, ay;
        NSString *rawWX = [self expandLayoutValue:[sec valueForKey:@"WindowX"]];
        NSString *rawWY = [self expandLayoutValue:[sec valueForKey:@"WindowY"]];
        e.windowXPercent = RMParsePositionValue(rawWX, &wx); e.windowX = wx;
        e.windowYPercent = RMParsePositionValue(rawWY, &wy); e.windowY = wy;
        e.windowXUnresolved = RMPositionLooksUnresolved(rawWX, wx, e.windowXPercent);
        e.windowYUnresolved = RMPositionLooksUnresolved(rawWY, wy, e.windowYPercent);
        e.anchorXPercent = RMParsePositionValue([self expandLayoutValue:[sec valueForKey:@"AnchorX"]], &ax); e.anchorX = ax;
        e.anchorYPercent = RMParsePositionValue([self expandLayoutValue:[sec valueForKey:@"AnchorY"]], &ay); e.anchorY = ay;

        NSString *drag = [sec valueForKey:@"Draggable"];
        e.draggable = drag == nil ? YES : ([drag integerValue] != 0);
        [entries addObject:e];
    }
    return entries;
}

// config is "root\\config\\sub"; variant selects the Nth .ini in the folder.
- (nullable RMLayoutEntry *)entryForConfig:(NSString *)config variant:(int)variant fm:(NSFileManager *)fm {
    NSArray<NSString *> *comps = [config componentsSeparatedByString:@"\\"];
    if (comps.count == 0) return nil;
    NSString *root = comps.firstObject;
    NSString *rootPath = [_skinsPath stringByAppendingPathComponent:root];

    NSString *folder = _skinsPath;
    for (NSString *c in comps) folder = [folder stringByAppendingPathComponent:c];

    BOOL isDir = NO;
    if (![fm fileExistsAtPath:folder isDirectory:&isDir] || !isDir) return nil;

    NSString *skinFile = [self chooseVariantInFolder:folder variant:variant fm:fm];
    if (skinFile == nil) return nil;

    RMLayoutEntry *e = [RMLayoutEntry new];
    e.skinFile = skinFile;
    e.rootConfigPath = rootPath;
    e.resourcesPath = [rootPath stringByAppendingPathComponent:@"@Resources"];
    e.configName = [config stringByReplacingOccurrencesOfString:@"\\" withString:@"\\"];
    e.draggable = YES;
    return e;
}

- (nullable NSString *)chooseVariantInFolder:(NSString *)folder variant:(int)variant fm:(NSFileManager *)fm {
    NSArray<NSString *> *contents = [fm contentsOfDirectoryAtPath:folder error:nil];
    NSMutableArray<NSString *> *inis = [NSMutableArray array];
    for (NSString *f in contents) {
        if ([f.pathExtension.lowercaseString isEqualToString:@"ini"]) [inis addObject:f];
    }
    if (inis.count == 0) return nil;
    [inis sortUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
        return [a caseInsensitiveCompare:b];
    }];
    NSInteger idx = variant - 1;
    if (idx < 0) idx = 0;
    if (idx >= (NSInteger)inis.count) idx = 0;
    return [folder stringByAppendingPathComponent:inis[(NSUInteger)idx]];
}

#pragma mark - Skin / fallback

- (NSArray<RMLayoutEntry *> *)skinEntries {
    // LoadType=Skin: Load is a path like "Folder\\Skin.ini" relative to Skins.
    NSString *rel = [self.load stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
    NSString *skinFile = [_skinsPath stringByAppendingPathComponent:rel];
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:skinFile]) return [self allSkinEntries];

    NSString *folder = [skinFile stringByDeletingLastPathComponent];
    // Root config = first path component under Skins.
    NSString *root = [rel componentsSeparatedByString:@"/"].firstObject ?: folder.lastPathComponent;
    NSString *rootPath = [_skinsPath stringByAppendingPathComponent:root];

    RMLayoutEntry *e = [RMLayoutEntry new];
    e.skinFile = skinFile;
    e.rootConfigPath = rootPath;
    e.resourcesPath = [rootPath stringByAppendingPathComponent:@"@Resources"];
    e.configName = self.load;
    e.windowX = 100; e.windowY = 100; e.draggable = YES;
    return @[e];
}

// Fallback: render every skin .ini found under Skins/<root>/... at cascading
// offsets when no usable layout exists.
- (NSArray<RMLayoutEntry *> *)allSkinEntries {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray<RMLayoutEntry *> *entries = [NSMutableArray array];
    NSArray<NSString *> *roots = [fm contentsOfDirectoryAtPath:_skinsPath error:nil];
    CGFloat offset = 60;
    for (NSString *root in roots) {
        NSString *rootPath = [_skinsPath stringByAppendingPathComponent:root];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:rootPath isDirectory:&isDir] || !isDir) continue;
        NSDirectoryEnumerator *en = [fm enumeratorAtPath:rootPath];
        for (NSString *rel in en) {
            if (![rel.pathExtension.lowercaseString isEqualToString:@"ini"]) continue;
            if ([rel.lowercaseString containsString:@"@resources"]) continue;
            RMLayoutEntry *e = [RMLayoutEntry new];
            e.skinFile = [rootPath stringByAppendingPathComponent:rel];
            e.rootConfigPath = rootPath;
            e.resourcesPath = [rootPath stringByAppendingPathComponent:@"@Resources"];
            e.configName = [NSString stringWithFormat:@"%@\\%@", root, rel];
            e.windowX = offset; e.windowY = offset; e.draggable = YES;
            offset += 40;
            [entries addObject:e];
        }
    }
    return entries;
}

@end
