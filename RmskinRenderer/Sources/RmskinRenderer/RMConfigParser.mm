#import "RMConfigParser.h"
#import "RMMathParser.h"
#import "RMLog.h"
#import <AppKit/AppKit.h>

@implementation RMConfigParser {
    NSMutableDictionary<NSString *, NSString *> *_variables;   // UPPER -> raw value
    NSMutableSet<NSString *> *_includedFiles;                  // cycle guard
    int _includeDepth;
}

- (instancetype)init {
    if ((self = [super init])) {
        _ini = [RMIniFile new];
        _variables = [NSMutableDictionary dictionary];
        _includedFiles = [NSMutableSet set];
        _resourcesPath = @"";
        _currentPath = @"";
        _currentFile = @"";
        _currentConfig = @"";
        _rootConfigPath = @"";
        _skinsPath = @"";
    }
    return self;
}

#pragma mark - Loading

- (BOOL)loadSkinFile:(NSString *)path {
    self.currentFile = path.lastPathComponent;
    if (self.currentPath.length == 0) {
        self.currentPath = [path stringByDeletingLastPathComponent];
    }
    BOOL ok = [self mergeFile:path relativeDir:[path stringByDeletingLastPathComponent]];
    // Read [Variables] (later-wins) after the full merge so includes are visible.
    [self readVariablesSection];
    return ok;
}

- (BOOL)mergeFile:(NSString *)path relativeDir:(NSString *)dir {
    NSString *std = path.stringByStandardizingPath;
    if ([_includedFiles containsObject:std.uppercaseString]) {
        RMLogWarn(@"skip re-included file: %@", path);
        return YES;
    }
    if (_includeDepth > 100) { RMLogWarn(@"include depth exceeded"); return NO; }
    [_includedFiles addObject:std.uppercaseString];

    RMIniFile *tmp = [RMIniFile new];
    if (![tmp parseContentsOfFile:std]) return NO;

    NSString *fileDir = [std stringByDeletingLastPathComponent];

    for (RMIniSection *section in tmp.sections) {
        RMIniSection *merged = [_ini ensureSectionNamed:section.name];
        BOOL isVariables = [section.name.uppercaseString isEqualToString:@"VARIABLES"];
        for (NSString *key in section.orderedKeys) {
            NSString *value = [section valueForKey:key] ?: @"";
            if ([key.uppercaseString hasPrefix:@"@INCLUDE"]) {
                // Variables defined so far (incl. #@#) are needed to resolve path.
                NSString *incRaw = [self expand:value];
                // Rainmeter uses '\' separators; normalise to POSIX '/'.
                incRaw = [incRaw stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
                NSString *incPath = incRaw;
                if (!incPath.isAbsolutePath) {
                    incPath = [fileDir stringByAppendingPathComponent:incRaw];
                }
                _includeDepth++;
                [self mergeFile:incPath relativeDir:fileDir];
                _includeDepth--;
                continue;
            }
            // Generic keys: first definition wins (Rainmeter insert-if-absent).
            [merged setValue:value forKey:key overwrite:NO];
            if (isVariables) {
                // Variables: later definitions override earlier ones.
                [self setVariable:key value:value];
            }
        }
    }
    return YES;
}

- (void)readVariablesSection {
    RMIniSection *vars = [_ini sectionNamed:@"Variables"];
    if (vars == nil) return;
    for (NSString *key in vars.orderedKeys) {
        if ([key.uppercaseString hasPrefix:@"@INCLUDE"]) continue;
        [self setVariable:key value:[vars valueForKey:key] ?: @""];
    }
}

#pragma mark - Variables

- (void)setVariable:(NSString *)name value:(NSString *)value {
    if (name.length == 0) return;
    _variables[name.uppercaseString] = value ?: @"";
}

- (nullable NSString *)builtInForUpper:(NSString *)upper {
    if ([upper isEqualToString:@"@"])            return self.resourcesPath;
    if ([upper isEqualToString:@"CURRENTPATH"])  return [self.currentPath hasSuffix:@"/"] ? self.currentPath : [self.currentPath stringByAppendingString:@"/"];
    if ([upper isEqualToString:@"CURRENTFILE"])  return self.currentFile;
    if ([upper isEqualToString:@"CURRENTCONFIG"]) return self.currentConfig;
    if ([upper isEqualToString:@"ROOTCONFIGPATH"]) return [self.rootConfigPath hasSuffix:@"/"] ? self.rootConfigPath : [self.rootConfigPath stringByAppendingString:@"/"];
    if ([upper isEqualToString:@"SKINSPATH"])    return [self.skinsPath hasSuffix:@"/"] ? self.skinsPath : [self.skinsPath stringByAppendingString:@"/"];
    if ([upper isEqualToString:@"CRLF"])         return @"\n";
    // Built-in monitor variables (macOS primary screen).
    if ([upper isEqualToString:@"WORKAREAX"] ||
        [upper isEqualToString:@"WORKAREAY"] ||
        [upper isEqualToString:@"WORKAREAWIDTH"] ||
        [upper isEqualToString:@"WORKAREAHEIGHT"] ||
        [upper isEqualToString:@"SCREENAREAWIDTH"] ||
        [upper isEqualToString:@"SCREENAREAHEIGHT"]) {
        NSScreen *screen = NSScreen.screens.firstObject;
        if (screen == nil) return @"0";
        NSRect visible = screen.visibleFrame;
        NSRect full    = screen.frame;
        if ([upper isEqualToString:@"WORKAREAX"])        return [NSString stringWithFormat:@"%g", NSMinX(visible)];
        if ([upper isEqualToString:@"WORKAREAY"])        return [NSString stringWithFormat:@"%g", NSMinY(visible)];
        if ([upper isEqualToString:@"WORKAREAWIDTH"])    return [NSString stringWithFormat:@"%g", NSWidth(visible)];
        if ([upper isEqualToString:@"WORKAREAHEIGHT"])   return [NSString stringWithFormat:@"%g", NSHeight(visible)];
        if ([upper isEqualToString:@"SCREENAREAWIDTH"])  return [NSString stringWithFormat:@"%g", NSWidth(full)];
        if ([upper isEqualToString:@"SCREENAREAHEIGHT"]) return [NSString stringWithFormat:@"%g", NSHeight(full)];
    }
    return nil;
}

- (nullable NSString *)variableForName:(NSString *)name {
    NSString *upper = name.uppercaseString;
    NSString *b = [self builtInForUpper:upper];
    if (b != nil) return b;
    return _variables[upper];
}

#pragma mark - Expansion

- (NSString *)expand:(NSString *)raw {
    if (raw.length == 0) return raw ?: @"";
    NSString *s = raw;
    // Bounded fixed-point iteration: variables may expand into further tokens.
    for (int pass = 0; pass < 20; pass++) {
        NSString *afterVars = [self expandVariablesOnce:s];
        NSString *afterSecs = [self expandSectionVariables:afterVars];
        if ([afterSecs isEqualToString:s]) { s = afterSecs; break; }
        s = afterSecs;
    }
    return s;
}

// Replace all #Name# occurrences once. #*text*# escapes to literal #text#.
- (NSString *)expandVariablesOnce:(NSString *)s {
    if ([s rangeOfString:@"#"].location == NSNotFound) return s;
    NSMutableString *out = [NSMutableString stringWithCapacity:s.length];
    NSUInteger i = 0, n = s.length;
    while (i < n) {
        unichar c = [s characterAtIndex:i];
        if (c != '#') { [out appendFormat:@"%C", c]; i++; continue; }
        // Find closing '#'.
        NSUInteger j = i + 1;
        // Escape form #*...*#
        if (j < n && [s characterAtIndex:j] == '*') {
            NSRange endEsc = [s rangeOfString:@"*#" options:0 range:NSMakeRange(j + 1, n - (j + 1))];
            if (endEsc.location != NSNotFound) {
                NSString *inner = [s substringWithRange:NSMakeRange(j + 1, endEsc.location - (j + 1))];
                [out appendFormat:@"#%@#", inner];
                i = endEsc.location + 2;
                continue;
            }
        }
        NSUInteger close = NSNotFound;
        for (NSUInteger k = i + 1; k < n; k++) {
            unichar ck = [s characterAtIndex:k];
            if (ck == '#') { close = k; break; }
            // Names are alnum/_/@; bail out on whitespace to avoid eating text.
            if (ck == ' ' || ck == '\t' || ck == '\n') break;
        }
        if (close == NSNotFound) { [out appendString:@"#"]; i++; continue; }
        NSString *name = [s substringWithRange:NSMakeRange(i + 1, close - i - 1)];
        NSString *val = [self variableForName:name];
        if (val != nil) {
            [out appendString:val];
        } else {
            // Unknown variable: drop to empty (Rainmeter behaviour), but keep a
            // truly empty token "##" as literal to avoid runaway loops.
            if (name.length == 0) [out appendString:@"#"];
        }
        i = close + 1;
    }
    return out;
}

// Resolve [Section] / [Section:selector] via the resolver, honouring nesting.
- (NSString *)expandSectionVariables:(NSString *)s {
    if (self.sectionVariableResolver == nil) return s;
    if ([s rangeOfString:@"["].location == NSNotFound) return s;

    NSMutableString *out = [NSMutableString stringWithCapacity:s.length];
    NSUInteger i = 0, n = s.length;
    while (i < n) {
        unichar c = [s characterAtIndex:i];
        if (c != '[') { [out appendFormat:@"%C", c]; i++; continue; }
        // Find matching ']' allowing one level via innermost scan.
        NSUInteger depth = 0, close = NSNotFound;
        for (NSUInteger k = i; k < n; k++) {
            unichar ck = [s characterAtIndex:k];
            if (ck == '[') depth++;
            else if (ck == ']') { depth--; if (depth == 0) { close = k; break; } }
        }
        if (close == NSNotFound) { [out appendFormat:@"%C", c]; i++; continue; }
        NSString *token = [s substringWithRange:NSMakeRange(i + 1, close - i - 1)];
        // Resolve nested tokens first.
        token = [self expandSectionVariables:token];
        NSString *resolved = nil;

        // Handle new-style section variable prefixes locally so they work even
        // before the sectionVariableResolver block is installed.
        if (token.length > 0) {
            unichar prefix = [token characterAtIndex:0];
            if (prefix == L'#') {
                // [#VariableName] — direct variable lookup.
                NSString *varName = [token substringFromIndex:1];
                resolved = [self variableForName:varName];
            } else if (prefix == L'$') {
                // [$Config:Key] — cross-skin reference, not supported.
                resolved = nil;
            }
        }

        if (resolved == nil && self.sectionVariableResolver) {
            resolved = self.sectionVariableResolver(token);
        }
        if (resolved != nil) {
            [out appendString:resolved];
        } else {
            [out appendFormat:@"[%@]", token];
        }
        i = close + 1;
    }
    return out;
}

#pragma mark - Typed reads

- (nullable NSString *)readString:(NSString *)section key:(NSString *)key default:(nullable NSString *)def {
    RMIniSection *s = [_ini sectionNamed:section];
    NSString *raw = [s valueForKey:key];
    if (raw == nil) return def;
    return [self expand:raw];
}

- (double)readDouble:(NSString *)section key:(NSString *)key default:(double)def {
    NSString *v = [self readString:section key:key default:nil];
    if (v == nil) return def;
    return [RMConfigParser evaluateNumber:v default:def];
}

- (int)readInt:(NSString *)section key:(NSString *)key default:(int)def {
    return (int)lround([self readDouble:section key:key default:(double)def]);
}

- (BOOL)readBool:(NSString *)section key:(NSString *)key default:(BOOL)def {
    NSString *v = [self readString:section key:key default:nil];
    if (v == nil) return def;
    v = [v stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if ([v caseInsensitiveCompare:@"1"] == NSOrderedSame) return YES;
    if ([v caseInsensitiveCompare:@"true"] == NSOrderedSame) return YES;
    if ([v caseInsensitiveCompare:@"0"] == NSOrderedSame) return NO;
    if ([v caseInsensitiveCompare:@"false"] == NSOrderedSame) return NO;
    return [RMConfigParser evaluateNumber:v default:(def ? 1 : 0)] != 0;
}

+ (double)evaluateNumber:(NSString *)expanded default:(double)def {
    NSString *v = [expanded stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if (v.length == 0) return def;
    // (formula) style — Rainmeter requires parentheses to trigger evaluation.
    if ([v hasPrefix:@"("] || [RMMathParser looksLikeFormula:v]) {
        double r = def;
        if ([RMMathParser parse:v result:&r]) return r;
    }
    // Plain number.
    double d = def;
    NSScanner *sc = [NSScanner scannerWithString:v];
    if ([sc scanDouble:&d]) return d;
    return def;
}

@end
