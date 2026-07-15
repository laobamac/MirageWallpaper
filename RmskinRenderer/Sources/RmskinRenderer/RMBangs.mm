#import "RMBangs.h"
#import "RMSkin.h"
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
        // [!WriteKeyValue Section Key Value [File]] — apply live (persist skipped).
        NSString *section = argAt(1), *key = argAt(2), *val = [cp expand:argAt(3)];
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
    } else if ([name isEqualToString:@"commandmeasure"] ||
               [name isEqualToString:@"pluginbang"] ||
               [name isEqualToString:@"setwindowposition"] ||
               [name isEqualToString:@"move"] ||
               [name isEqualToString:@"zpos"] ||
               [name isEqualToString:@"snapedges"] ||
               [name isEqualToString:@"draggable"] ||
               [name isEqualToString:@"keeponscreen"] ||
               [name isEqualToString:@"clickthrough"] ||
               [name isEqualToString:@"activateconfig"] ||
               [name isEqualToString:@"deactivateconfig"] ||
               [name isEqualToString:@"toggleconfig"] ||
               [name isEqualToString:@"skincustommenu"] ||
               [name isEqualToString:@"setwallpaper"] ||
               [name isEqualToString:@"settransparency"] ||
               [name isEqualToString:@"setwindowtransparency"] ||
               [name isEqualToString:@"setanchor"] ||
               [name hasPrefix:@"fade"] ||
               [name hasPrefix:@"showfade"] || [name hasPrefix:@"hidefade"] ||
               [name isEqualToString:@"toggle"]) {
        // Window/config/plugin management bangs that have no effect in the
        // standalone floating-widget host; accepted so actions don't error.
        [skin requestRedraw];
    } else {
        RMLogDebug(@"unhandled bang: %@", name);
    }
}

@end
