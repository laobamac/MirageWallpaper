#import "RMBangs.h"
#import "RMSkin.h"
#import "RMSkinView.h"
#import "RMConfigParser.h"
#import "RMMathParser.h"
#import "RMMeter.h"
#import "RMMeasure.h"
#import "RMLog.h"

@implementation RMBangs

// Split "[!A x][!B y z]" into individual "!A x" / "!B y z" strings.
+ (NSArray<NSString *> *)splitGroups:(NSString *)actions {
    NSMutableArray<NSString *> *groups = [NSMutableArray array];
    NSUInteger i = 0, n = actions.length;
    while (i < n) {
        unichar c = [actions characterAtIndex:i];
        if (c == '[') {
            NSInteger depth = 0; NSUInteger k = i;
            for (; k < n; k++) {
                unichar ck = [actions characterAtIndex:k];
                if (ck == '[') depth++;
                else if (ck == ']') { depth--; if (depth == 0) break; }
            }
            if (k < n) {
                NSString *inner = [actions substringWithRange:NSMakeRange(i + 1, k - i - 1)];
                [groups addObject:inner];
                i = k + 1;
                continue;
            }
        }
        i++;
    }
    if (groups.count == 0 && actions.length) [groups addObject:actions];
    return groups;
}

// Tokenize a bang group into ["!SetVariable", "Scale", "1"], honouring quotes.
+ (NSArray<NSString *> *)tokenize:(NSString *)group {
    NSMutableArray<NSString *> *toks = [NSMutableArray array];
    NSMutableString *cur = [NSMutableString string];
    BOOL inQuote = NO, has = NO; unichar q = '"';
    NSUInteger i = 0, n = group.length;
    while (i < n) {
        unichar c = [group characterAtIndex:i];
        if (inQuote) {
            if (c == q) inQuote = NO;
            else { [cur appendFormat:@"%C", c]; has = YES; }
        } else if (c == '"' || c == '\'') {
            inQuote = YES; has = YES; q = c;
        } else if (c == ' ' || c == '\t') {
            if (has) { [toks addObject:cur.copy]; [cur setString:@""]; has = NO; }
        } else {
            [cur appendFormat:@"%C", c]; has = YES;
        }
        i++;
    }
    if (has) [toks addObject:cur.copy];
    return toks;
}

+ (void)execute:(NSString *)actions onSkin:(RMSkin *)skin {
    if (actions.length == 0 || skin == nil) return;
    for (NSString *group in [self splitGroups:actions]) {
        NSArray<NSString *> *toks = [self tokenize:group];
        if (toks.count == 0) continue;
        NSString *bang = toks[0];
        if (![bang hasPrefix:@"!"]) {
            // A non-bang group is a "run" action: a program, file or URL, e.g.
            // ["https://example.com"] or ["C:\App.exe" -flag]. Handle what makes
            // sense on macOS; Windows-only targets are skipped gracefully.
            [self runCommandGroup:toks onSkin:skin];
            continue;
        }
        NSString *rawName = [bang substringFromIndex:1].lowercaseString;
        // "!RainmeterShowMeter" is an old alias for "!ShowMeter" etc.
        NSString *name = rawName;
        if ([name hasPrefix:@"rainmeter"]) name = [name substringFromIndex:9];
        [self run:name args:toks onSkin:skin];
    }
}

// Open a URL/file target on macOS. Windows-specific paths (drive letters,
// %env%, shell:) have no macOS equivalent and are ignored.
+ (void)runCommandGroup:(NSArray<NSString *> *)toks onSkin:(RMSkin *)skin {
    NSString *target = [skin.parser expand:toks.firstObject];
    target = [target stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (target.length == 0) return;
    NSString *low = target.lowercaseString;
    // Web / mail / custom URL schemes.
    if ([low hasPrefix:@"http://"] || [low hasPrefix:@"https://"] ||
        [low hasPrefix:@"mailto:"] || [low hasPrefix:@"ftp://"]) {
        NSURL *u = [NSURL URLWithString:target];
        if (u) [[NSWorkspace sharedWorkspace] openURL:u];
        return;
    }
    // Windows-isms we cannot honour on macOS.
    if ([low hasPrefix:@"shell:"] || [target rangeOfString:@"%"].location != NSNotFound ||
        (target.length >= 2 && [target characterAtIndex:1] == ':')) {
        RMLogDebug(@"skip non-macOS command: %@", target);
        return;
    }
    // A path that actually exists on disk: open it.
    if ([[NSFileManager defaultManager] fileExistsAtPath:target]) {
        [[NSWorkspace sharedWorkspace] openFile:target];
        return;
    }
    RMLogDebug(@"unresolved command: %@", target);
}

+ (void)run:(NSString *)name args:(NSArray<NSString *> *)toks onSkin:(RMSkin *)skin {
    RMConfigParser *cp = skin.parser;
    auto argAt = ^NSString *(NSUInteger i) { return i < toks.count ? toks[i] : @""; };

    if ([name isEqualToString:@"setvariable"]) {
        NSString *var = argAt(1);
        NSString *val = [cp expand:argAt(2)];
        double num;
        if ([RMMathParser parse:val result:&num]) {
            // Store a clean numeric string when the value evaluates.
            val = (num == floor(num)) ? [NSString stringWithFormat:@"%ld", (long)num]
                                      : [NSString stringWithFormat:@"%g", num];
        }
        [cp setVariable:var value:val];
        [skin requestRedraw];
    } else if ([name isEqualToString:@"writekeyvalue"]) {
        // [!WriteKeyValue Section Key Value [File]] — apply live AND persist to disk.
        NSString *section = argAt(1), *key = argAt(2), *val = [cp expand:argAt(3)];
        NSString *file = argAt(4);
        double num;
        if ([RMMathParser parse:val result:&num]) {
            val = (num == floor(num)) ? [NSString stringWithFormat:@"%ld", (long)num]
                                      : [NSString stringWithFormat:@"%g", num];
        }
        if ([section.uppercaseString isEqualToString:@"VARIABLES"]) {
            [cp setVariable:key value:val];
        } else {
            [skin setOption:key value:val forSection:section];
        }
        // Persist to disk: write the key=value line into the target .ini file.
        // If no File is specified, use the skin's own .ini.
        NSString *targetFile = file.length ? [cp expand:file] : cp.currentFile;
        if (targetFile.length) {
            if (![targetFile isAbsolutePath]) {
                targetFile = [cp.currentPath stringByAppendingPathComponent:targetFile];
            }
            targetFile = [targetFile stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
            [RMBangs writeKey:key value:val section:section toFile:targetFile];
        }
        [skin requestRedraw];
    } else if ([name isEqualToString:@"refresh"] || [name isEqualToString:@"refreshapp"]) {
        [skin reload];
        [skin tick];
        [skin requestRedraw];
    } else if ([name isEqualToString:@"redraw"]) {
        [skin requestRedraw];
    } else if ([name isEqualToString:@"update"]) {
        [skin tick];
        [skin requestRedraw];
    } else if ([name isEqualToString:@"updatemeter"] ||
               [name isEqualToString:@"updatemeasure"]) {
        // We update the whole skin on tick; a targeted update just redraws.
        [skin requestRedraw];
    } else if ([name isEqualToString:@"updatemetergroup"] ||
               [name isEqualToString:@"updatemeasuregroup"]) {
        [skin requestRedraw];
    } else if ([name isEqualToString:@"hidemeter"] || [name isEqualToString:@"hide"]) {
        for (NSUInteger i = 1; i < toks.count; i++) {
            RMMeter *m = [skin meterNamed:argAt(i)]; m.hidden = YES;
        }
        [skin requestRedraw];
    } else if ([name isEqualToString:@"showmeter"] || [name isEqualToString:@"show"]) {
        for (NSUInteger i = 1; i < toks.count; i++) {
            RMMeter *m = [skin meterNamed:argAt(i)]; m.hidden = NO;
        }
        [skin requestRedraw];
    } else if ([name isEqualToString:@"togglemeter"] || [name isEqualToString:@"toggle"]) {
        RMMeter *m = [skin meterNamed:argAt(1)];
        m.hidden = !m.hidden; [skin requestRedraw];
    } else if ([name isEqualToString:@"hidemetergroup"]) {
        for (RMMeter *m in [skin metersInGroup:argAt(1)]) m.hidden = YES;
        [skin requestRedraw];
    } else if ([name isEqualToString:@"showmetergroup"]) {
        for (RMMeter *m in [skin metersInGroup:argAt(1)]) m.hidden = NO;
        [skin requestRedraw];
    } else if ([name isEqualToString:@"togglemetergroup"]) {
        for (RMMeter *m in [skin metersInGroup:argAt(1)]) m.hidden = !m.hidden;
        [skin requestRedraw];
    } else if ([name isEqualToString:@"setoption"]) {
        [skin setOption:argAt(2) value:[cp expand:argAt(3)] forSection:argAt(1)];
        [skin requestRedraw];
    } else if ([name isEqualToString:@"setoptiongroup"]) {
        for (RMMeter *m in [skin metersInGroup:argAt(1)]) {
            [skin setOption:argAt(2) value:[cp expand:argAt(3)] forSection:m.name];
        }
        [skin requestRedraw];
    } else if ([name isEqualToString:@"execute"]) {
        // !Execute groups nested bang groups: re-enter with the remaining args joined.
        NSMutableString *inner = [NSMutableString string];
        for (NSUInteger i = 1; i < toks.count; i++) {
            [inner appendString:toks[i]];
            if (i + 1 < toks.count) [inner appendString:@" "];
        }
        [self execute:inner onSkin:skin];
    } else if ([name isEqualToString:@"disablemeasure"]) {
        for (NSUInteger i = 1; i < toks.count; i++) {
            RMMeasure *ms = [skin measureNamed:argAt(i)]; ms.disabled = YES;
        }
        [skin tick]; [skin requestRedraw];
    } else if ([name isEqualToString:@"enablemeasure"]) {
        for (NSUInteger i = 1; i < toks.count; i++) {
            RMMeasure *ms = [skin measureNamed:argAt(i)]; ms.disabled = NO;
        }
        [skin tick]; [skin requestRedraw];
    } else if ([name isEqualToString:@"togglemeasure"]) {
        RMMeasure *ms = [skin measureNamed:argAt(1)];
        ms.disabled = !ms.disabled; [skin tick]; [skin requestRedraw];
    } else if ([name isEqualToString:@"disablemeasuregroup"]) {
        for (RMMeasure *m in [skin measuresInGroup:argAt(1)]) m.disabled = YES;
        [skin tick]; [skin requestRedraw];
    } else if ([name isEqualToString:@"enablemeasuregroup"]) {
        for (RMMeasure *m in [skin measuresInGroup:argAt(1)]) m.disabled = NO;
        [skin tick]; [skin requestRedraw];
    } else if ([name isEqualToString:@"togglemeasuregroup"]) {
        for (RMMeasure *m in [skin measuresInGroup:argAt(1)]) m.disabled = !m.disabled;
        [skin tick]; [skin requestRedraw];
    } else if ([name isEqualToString:@"setclip"]) {
        [skin requestRedraw];
    } else if ([name isEqualToString:@"settransparency"] ||
               [name isEqualToString:@"setwindowtransparency"]) {
        // [!SetTransparency Alpha] — set window opacity (0=invisible, 255=opaque).
        double alpha = [RMConfigParser evaluateNumber:[cp expand:argAt(1)] default:255] / 255.0;
        alpha = MAX(0, MIN(1, alpha));
        skin.windowAlpha = alpha;
    } else if ([name isEqualToString:@"clickthrough"]) {
        // [!ClickThrough 1|0] — window ignores mouse events.
        BOOL ct = [cp readBoolFromExpanded:[cp expand:argAt(1)] default:NO];
        skin.clickThrough = ct;
    } else if ([name isEqualToString:@"fade"] ||
               [name isEqualToString:@"showfade"] || [name isEqualToString:@"hidefade"]) {
        // [!ShowFade / !HideFade / !Fade Config Alpha Duration]
        // ShowFade: fade in the window; HideFade: fade out.
        double alpha = 1.0;
        NSTimeInterval duration = 0.5;
        if ([name isEqualToString:@"showfade"]) {
            alpha = 1.0;
        } else if ([name isEqualToString:@"hidefade"]) {
            alpha = 0.0;
        } else {
            // !Fade — check for alpha/duration args.
            NSString *alphaArg = argAt(1);
            NSString *durArg = argAt(2);
            if (alphaArg.length > 0) {
                alpha = [RMConfigParser evaluateNumber:[cp expand:alphaArg] default:255] / 255.0;
                alpha = MAX(0, MIN(1, alpha));
            }
            if (durArg.length > 0) {
                double d = [RMConfigParser evaluateNumber:[cp expand:durArg] default:0];
                if (d > 0) duration = d / 1000.0; // milliseconds to seconds
            }
        }
        if (duration > 0.01) {
            skin.fadeDuration = duration;
        }
        skin.windowAlpha = alpha;
    } else if ([name isEqualToString:@"commandmeasure"] ||
               [name isEqualToString:@"pluginbang"]) {
        // Plugin/CommandMeasure bang: forward to the named measure if it supports it.
        NSString *measureName = argAt(1);
        RMMeasure *m = [skin measureNamed:measureName];
        if (m) {
            NSString *args = argAt(2);
            if (args.length) {
                RMLogDebug(@"plugin bang forwarded to %@: %@", measureName, args);
            }
        }
    } else if ([name isEqualToString:@"setwindowposition"] ||
               [name isEqualToString:@"move"]) {
        // [!Move X Y] — move the widget window.
        double x = [RMConfigParser evaluateNumber:[cp expand:argAt(1)] default:0];
        double y = [RMConfigParser evaluateNumber:[cp expand:argAt(2)] default:0];
        skin.windowPosition = NSMakePoint(x, y);
    } else if ([name isEqualToString:@"zpos"] ||
               [name isEqualToString:@"setanchor"] ||
               [name isEqualToString:@"snapedges"] ||
               [name isEqualToString:@"draggable"] ||
               [name isEqualToString:@"keeponscreen"] ||
               [name isEqualToString:@"activateconfig"] ||
               [name isEqualToString:@"deactivateconfig"] ||
               [name isEqualToString:@"toggleconfig"] ||
               [name isEqualToString:@"skincustommenu"] ||
               [name isEqualToString:@"setwallpaper"]) {
        // Window/config management bangs that are no-ops in the standalone
        // floating-widget host; accepted so actions don't error.
        [skin requestRedraw];
    } else {
        RMLogDebug(@"unhandled bang: %@", name);
    }
}

#pragma mark - File persistence

// Write/update a key=value line in a section of a Rainmeter .ini file on disk.
// Attempts a minimal in-place edit: if the key already exists in the target
// section, the line is replaced; otherwise the key is appended at the end of
// the section. Skips write if the file cannot be read or written.
+ (void)writeKey:(NSString *)key
          value:(NSString *)value
        section:(NSString *)section
         toFile:(NSString *)filePath {
    if (key.length == 0 || section.length == 0 || filePath.length == 0) return;

    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:filePath]) return;

    NSError *err = nil;
    NSString *content = [NSString stringWithContentsOfFile:filePath
                                                 encoding:NSUTF8StringEncoding
                                                    error:&err];
    if (err) {
        // Try Latin-1 fallback.
        content = [NSString stringWithContentsOfFile:filePath
                                           encoding:NSISOLatin1StringEncoding
                                              error:&err];
    }
    if (err || content == nil) {
        RMLogWarn(@"WriteKeyValue: cannot read %@: %@", filePath, err);
        return;
    }

    NSArray<NSString *> *lines = [content componentsSeparatedByString:@"\n"];
    NSMutableArray<NSString *> *newLines = [NSMutableArray new];
    BOOL inTargetSection = NO;
    BOOL wroteKey = NO;
    NSString *sectionHeader = [NSString stringWithFormat:@"[%@]", section];

    for (NSUInteger i = 0; i < lines.count; i++) {
        NSString *line = lines[i];
        NSString *trimmed = [line stringByTrimmingCharactersInSet:
                             [NSCharacterSet whitespaceCharacterSet]];

        // Detect section boundaries.
        if ([trimmed hasPrefix:@"["] && [trimmed hasSuffix:@"]"]) {
            // If we were in the target section and didn't write the key, append it.
            if (inTargetSection && !wroteKey) {
                [newLines addObject:[NSString stringWithFormat:@"%@=%@", key, value]];
                wroteKey = YES;
            }
            inTargetSection = [trimmed caseInsensitiveCompare:sectionHeader] == NSOrderedSame;
            [newLines addObject:line];
            continue;
        }

        // Inside the target section: replace or skip existing key.
        if (inTargetSection) {
            NSRange eq = [line rangeOfString:@"="];
            if (eq.location != NSNotFound) {
                NSString *lineKey = [[line substringToIndex:eq.location]
                    stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                if ([lineKey caseInsensitiveCompare:key] == NSOrderedSame) {
                    [newLines addObject:[NSString stringWithFormat:@"%@=%@", key, value]];
                    wroteKey = YES;
                    continue;
                }
            }
        }
        [newLines addObject:line];
    }

    // If the target section was the last one and key wasn't written, append.
    if (inTargetSection && !wroteKey) {
        [newLines addObject:[NSString stringWithFormat:@"%@=%@", key, value]];
        wroteKey = YES;
    }

    // If the section doesn't exist at all, append it at the end.
    if (!wroteKey) {
        [newLines addObject:@""];
        [newLines addObject:sectionHeader];
        [newLines addObject:[NSString stringWithFormat:@"%@=%@", key, value]];
    }

    NSString *output = [newLines componentsJoinedByString:@"\n"];
    [output writeToFile:filePath atomically:YES encoding:NSUTF8StringEncoding error:&err];
    if (err) {
        RMLogWarn(@"WriteKeyValue: write failed %@: %@", filePath, err);
    }
}

@end
