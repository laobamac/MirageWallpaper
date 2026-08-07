#import "RMMeasure.h"
#import "RMConfigParser.h"
#import "RMMathParser.h"
#import "RMLog.h"
#import <AppKit/AppKit.h>

#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>

#pragma mark - Substitute rule

@interface RMSubstituteRule : NSObject
@property (nonatomic, copy) NSString *find;
@property (nonatomic, copy) NSString *replace;
@end
@implementation RMSubstituteRule
@end

#pragma mark - Base

@interface RMMeasure () {
    // Previous state of each If*/IfCondition test, for edge-triggered firing.
    BOOL _aboveState, _belowState, _equalState;
    BOOL _condState[16];
    BOOL _condPrimed;
}
@property (nonatomic, copy)   NSString *rawType;
@property (nonatomic, strong) NSArray<RMSubstituteRule *> *substitutes;
@property (nonatomic, assign) BOOL regexpSubstitute;
@property (nonatomic, strong, nullable) NSDictionary *conditionOptions;
@property (nonatomic, assign) BOOL ifConditionMode;   // 1 = level-triggered (fire every update)
@end

// Forward declarations of the concrete measure classes.
@interface RMMeasureTime   : RMMeasure @end
@interface RMMeasureCalc   : RMMeasure @end
@interface RMMeasureCPU    : RMMeasure @end
@interface RMMeasureMemory : RMMeasure @end
@interface RMMeasureDisk   : RMMeasure @end
@interface RMMeasureUptime : RMMeasure @end
@interface RMMeasureNet    : RMMeasure @end
@interface RMMeasureNowPlaying : RMMeasure @end
@interface RMMeasureBattery    : RMMeasure @end
@interface RMMeasureSysInfo    : RMMeasure @end
@interface RMMeasureProcess    : RMMeasure @end
@interface RMMeasureWebParser  : RMMeasure @end
@interface RMMeasureRunCommand : RMMeasure @end
@interface RMMeasureFolderInfo : RMMeasure @end
@interface RMMeasureRegistry   : RMMeasure @end
@interface RMMeasureRecycleManager : RMMeasure @end
@interface RMMeasureWiFiStatus : RMMeasure @end
@interface RMMeasureString     : RMMeasure @end
@interface RMMeasureStub   : RMMeasure @end

@implementation RMMeasure

- (BOOL)isStringMeasure { return NO; }

+ (nullable RMMeasure *)measureWithType:(NSString *)type
                                   name:(NSString *)name
                                 parser:(RMConfigParser *)parser {
    NSString *t = type.lowercaseString;
    Class cls;
    if ([t isEqualToString:@"time"])                       cls = [RMMeasureTime class];
    else if ([t isEqualToString:@"calc"])                  cls = [RMMeasureCalc class];
    else if ([t isEqualToString:@"cpu"])                   cls = [RMMeasureCPU class];
    else if ([t isEqualToString:@"physicalmemory"] ||
             [t isEqualToString:@"memory"] ||
             [t isEqualToString:@"swapmemory"])            cls = [RMMeasureMemory class];
    else if ([t isEqualToString:@"freediskspace"])         cls = [RMMeasureDisk class];
    else if ([t isEqualToString:@"uptime"])                cls = [RMMeasureUptime class];
    else if ([t isEqualToString:@"net"] ||
             [t isEqualToString:@"netin"] ||
             [t isEqualToString:@"netout"])                cls = [RMMeasureNet class];
    else if ([t isEqualToString:@"nowplaying"])            cls = [RMMeasureNowPlaying class];
    else if ([t isEqualToString:@"battery"] ||
             [t isEqualToString:@"power"])                 cls = [RMMeasureBattery class];
    else if ([t isEqualToString:@"sysinfo"])               cls = [RMMeasureSysInfo class];
    else if ([t isEqualToString:@"process"] ||
             [t isEqualToString:@"advancedcpu"])            cls = [RMMeasureProcess class];
    else if ([t isEqualToString:@"webparser"])              cls = [RMMeasureWebParser class];
    else if ([t isEqualToString:@"runcommand"])             cls = [RMMeasureRunCommand class];
    else if ([t isEqualToString:@"folderinfo"])             cls = [RMMeasureFolderInfo class];
    else if ([t isEqualToString:@"registry"])               cls = [RMMeasureRegistry class];
    else if ([t isEqualToString:@"recyclemanager"])         cls = [RMMeasureRecycleManager class];
    else if ([t isEqualToString:@"wifistatus"])             cls = [RMMeasureWiFiStatus class];
    else if ([t isEqualToString:@"string"])                 cls = [RMMeasureString class];
    else if ([t isEqualToString:@"script"])                 cls = [RMMeasureStub class];  // Lua: not yet implemented
    else if ([t isEqualToString:@"plugin"])                 cls = [RMMeasureStub class];  // Plugin: not yet implemented
    else if ([t isEqualToString:@"coretemp"] ||
             [t isEqualToString:@"speedfan"] ||
             [t isEqualToString:@"inputtext"])              cls = [RMMeasureStub class];  // Windows-specific
    else                                                   cls = [RMMeasureStub class];

    RMMeasure *m = [[cls alloc] init];
    m.name = name;
    m.parser = parser;
    m.rawType = t;
    return m;
}

- (instancetype)init {
    if ((self = [super init])) {
        _minValue = 0;
        _maxValue = 1;
        _updateDivider = 1;
        _defaultUpdateDivider = 1;
        _updateCounter = 0;
    }
    return self;
}

- (void)readOptions {
    RMConfigParser *cp = self.parser;
    self.disabled = [cp readBool:self.name key:@"Disabled" default:NO];
    self.invert   = [cp readBool:self.name key:@"InvertMeasure" default:NO];
    // Use the section's explicit UpdateDivider if present; otherwise fall back
    // to the [Rainmeter] DefaultUpdateDivider (already set on the measure).
    int explicitDivider = [cp readInt:self.name key:@"UpdateDivider" default:-1];
    self.updateDivider = (explicitDivider >= 1) ? explicitDivider : self.defaultUpdateDivider;
    if (self.updateDivider < 1) self.updateDivider = 1;
    self.group    = [cp readString:self.name key:@"Group" default:nil];

    NSString *minS = [cp readString:self.name key:@"MinValue" default:nil];
    if (minS) self.minValue = [RMConfigParser evaluateNumber:minS default:0];
    NSString *maxS = [cp readString:self.name key:@"MaxValue" default:nil];
    if (maxS) self.maxValue = [RMConfigParser evaluateNumber:maxS default:1];

    self.regexpSubstitute = [cp readBool:self.name key:@"RegExpSubstitute" default:NO];
    NSString *subs = [[cp.ini sectionNamed:self.name] valueForKey:@"Substitute"];
    if (subs.length) {
        unichar f = [subs characterAtIndex:0];
        unichar l = [subs characterAtIndex:subs.length - 1];
        BOOL mismatched = ((f == '"' && l == '\'') || (f == '\'' && l == '"'));
        if (!mismatched) subs = [NSString stringWithFormat:@"\"%@\"", subs];
    }
    self.substitutes = [self parseSubstitute:subs];

    [self readSubclassOptions];
    [self readConditionOptions];
}

// Read If*Action / IfCondition / IfTrue/FalseAction options into a cached dict.
- (void)readConditionOptions {
    RMConfigParser *cp = self.parser;
    RMIniSection *sec = [cp.ini sectionNamed:self.name];
    NSMutableDictionary *cond = [NSMutableDictionary dictionary];
    NSDictionary *kmap = @{
        @"IfAboveValue":@"aboveVal", @"IfAboveAction":@"aboveAct",
        @"IfBelowValue":@"belowVal", @"IfBelowAction":@"belowAct",
        @"IfEqualValue":@"equalVal", @"IfEqualAction":@"equalAct",
        @"IfMatchAction":@"matchAct",
    };
    for (NSString *k in kmap) {
        NSString *v = [sec valueForKey:k];
        if (v.length) cond[kmap[k]] = v;
    }
    // IfCondition / IfCondition2 ... with IfTrueAction / IfFalseAction.
    NSMutableArray<NSDictionary *> *ifConds = [NSMutableArray array];
    for (int i = 1; i <= 16; i++) {
        NSString *cKey = (i == 1) ? @"IfCondition" : [NSString stringWithFormat:@"IfCondition%d", i];
        NSString *tKey = (i == 1) ? @"IfTrueAction" : [NSString stringWithFormat:@"IfTrueAction%d", i];
        NSString *fKey = (i == 1) ? @"IfFalseAction" : [NSString stringWithFormat:@"IfFalseAction%d", i];
        NSString *c = [sec valueForKey:cKey];
        if (c.length == 0) { if (i > 1) break; else continue; }
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        d[@"cond"] = c;
        NSString *t = [sec valueForKey:tKey]; if (t.length) d[@"true"] = t;
        NSString *f = [sec valueForKey:fKey]; if (f.length) d[@"false"] = f;
        [ifConds addObject:d];
    }
    if (ifConds.count) cond[@"ifConds"] = ifConds;

    // IfConditionMode=1 fires the action on every update the condition holds;
    // the Rainmeter default (0) fires only once when the condition becomes true.
    self.ifConditionMode = [cp readBool:self.name key:@"IfConditionMode" default:NO];

    _conditionOptions = cond.count ? cond : nil;
    // Reset edge-trigger state so the first update after (re)load re-evaluates.
    _aboveState = _belowState = _equalState = NO;
    for (int i = 0; i < 16; i++) _condState[i] = NO;
    _condPrimed = NO;
}

- (void)readSubclassOptions { /* override */ }

- (void)update {
    if (self.disabled) return;
    self.updateCounter++;
    if (self.updateCounter < self.updateDivider) return;
    self.updateCounter = 0;
    [self updateValue];
    if (self.invert && self.maxValue > self.minValue) {
        self.value = self.maxValue - (self.value - self.minValue);
    }
    [self fireConditionActions];
}

- (void)fireConditionActions {
    if (_conditionOptions.count == 0 || self.executeAction == nil) return;
    double v = self.value;
    // Wrap bare !bang strings in [] so RMBangs.splitGroups can parse them, then
    // execute. `level` == YES means IfConditionMode: fire every update.
    BOOL level = self.ifConditionMode;
    auto fire = ^(NSString *raw) {
        if (raw.length == 0) return;
        NSString *wrapped = ([raw hasPrefix:@"["] ? raw
                              : [NSString stringWithFormat:@"[%@]", raw]);
        self.executeAction(wrapped);
    };
    // Edge-triggered helper: fire `act` only when `now` transitions NO->YES
    // (unless level-triggered). Updates *state BEFORE firing so a re-entrant
    // tick triggered by the action (e.g. !Update) sees the new state and does
    // not recurse infinitely.
    auto edge = ^(BOOL now, BOOL *state, NSString *act) {
        BOOL prev = *state;
        *state = now;
        if (now && (level || !prev)) fire(act);
    };

    NSString *aboveValS = _conditionOptions[@"aboveVal"];
    NSString *belowValS = _conditionOptions[@"belowVal"];
    NSString *equalValS = _conditionOptions[@"equalVal"];
    if (aboveValS.length) {
        double t = [RMConfigParser evaluateNumber:aboveValS default:0];
        edge(v > t, &_aboveState, _conditionOptions[@"aboveAct"]);
    }
    if (belowValS.length) {
        double t = [RMConfigParser evaluateNumber:belowValS default:0];
        edge(v < t, &_belowState, _conditionOptions[@"belowAct"]);
    }
    if (equalValS.length) {
        double t = [RMConfigParser evaluateNumber:equalValS default:0];
        edge(fabs(v - t) < 0.001, &_equalState, _conditionOptions[@"equalAct"]);
    }

    // IfCondition formulas: evaluate each, fire True/False action on transition.
    NSArray<NSDictionary *> *ifConds = _conditionOptions[@"ifConds"];
    if (ifConds.count) {
        RMConfigParser *cp = self.parser;
        RMMathVariableResolver resolver = ^BOOL(NSString *name, double *out) {
            if (cp.measureValueResolver) return cp.measureValueResolver(name, out);
            return NO;
        };
        BOOL primed = _condPrimed;
        for (NSUInteger i = 0; i < ifConds.count && i < 16; i++) {
            NSDictionary *d = ifConds[i];
            NSString *expanded = [cp expand:d[@"cond"]];
            double r = 0;
            BOOL truth = NO;
            if ([RMMathParser parse:expanded variableResolver:resolver result:&r]) truth = (r != 0);
            BOOL prev = _condState[i];
            _condState[i] = truth;
            // Only fire on transition (or every update in level mode); when the
            // condition state hasn't been primed yet, treat the first evaluation
            // as a transition so initial True/False actions run once.
            BOOL changed = (!primed) || (truth != prev);
            if (level || changed) {
                fire(truth ? d[@"true"] : d[@"false"]);
            }
        }
        _condPrimed = YES;
    }
}

- (void)updateValue { /* override */ }

- (nullable NSString *)rawString { return nil; }

- (double)numericValue {
    // A string measure whose text is numeric (e.g. Time "%d" → "15") should
    // resolve to that number when referenced by a Calc formula.
    NSString *s = [self rawString];
    if (s.length) {
        NSScanner *sc = [NSScanner scannerWithString:s];
        double d = 0;
        if ([sc scanDouble:&d]) return d;
    }
    return self.value;
}

- (NSString *)displayStringAutoScale:(BOOL)autoScale
                            decimals:(int)decimals
                          percentual:(BOOL)percentual
                               scale:(double)scale {
    if (self.isStringMeasure) {
        return [self applySubstitute:([self rawString] ?: @"")];
    }
    double v = self.value;
    if (percentual && self.maxValue > self.minValue) {
        v = (v - self.minValue) / (self.maxValue - self.minValue) * 100.0;
    } else if (scale != 0 && scale != 1) {
        v = v * scale;
    }
    NSString *out;
    if (autoScale && !percentual) {
        out = [RMMeasure autoScaleString:v decimals:(decimals < 0 ? 1 : decimals)];
    } else {
        int d = decimals < 0 ? 0 : decimals;
        out = [NSString stringWithFormat:@"%.*f", d, v];
    }
    return [self applySubstitute:out];
}

+ (NSString *)autoScaleString:(double)v decimals:(int)decimals {
    static const char *suffix[] = {"", "k", "M", "G", "T", "P"};
    int i = 0;
    double a = fabs(v);
    while (a >= 1024.0 && i < 5) { a /= 1024.0; v /= 1024.0; i++; }
    return [NSString stringWithFormat:@"%.*f%s", decimals, v, suffix[i]];
}

#pragma mark Substitute

- (NSArray<RMSubstituteRule *> *)parseSubstitute:(nullable NSString *)spec {
    if (spec.length == 0) return @[];
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    NSMutableString *cur = [NSMutableString string];
    BOOL inQuote = NO, sawQuote = NO; unichar q = '"';
    NSUInteger i = 0, n = spec.length;
    while (i < n) {
        unichar c = [spec characterAtIndex:i];
        if (inQuote) {
            if (c == q) { inQuote = NO; }
            else [cur appendFormat:@"%C", c];
        } else {
            if (c == '"' || c == '\'') { inQuote = YES; sawQuote = YES; q = c; }
            else if (c == ':' || c == ',') { [tokens addObject:cur.copy]; [cur setString:@""]; sawQuote = NO; }
            else if (c != ' ' && c != '\t') { if (!sawQuote) [cur appendFormat:@"%C", c]; }
        }
        i++;
    }
    [tokens addObject:cur.copy];
    NSMutableArray<RMSubstituteRule *> *rules = [NSMutableArray array];
    for (NSUInteger k = 0; k + 1 < tokens.count; k += 2) {
        RMSubstituteRule *r = [RMSubstituteRule new];
        r.find = tokens[k];
        r.replace = tokens[k + 1];
        [rules addObject:r];
    }
    return rules;
}

- (NSString *)applySubstitute:(NSString *)input {
    if (input == nil) return @"";
    if (self.substitutes.count == 0) return input;
    NSString *s = input;
    for (RMSubstituteRule *r in self.substitutes) {
        if (self.regexpSubstitute) {
            NSError *e = nil;
            NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:r.find
                                                                                options:0 error:&e];
            if (re) {
                NSString *tmpl = r.replace;
                tmpl = [tmpl stringByReplacingOccurrencesOfString:@"\\1" withString:@"$1"];
                tmpl = [tmpl stringByReplacingOccurrencesOfString:@"\\2" withString:@"$2"];
                s = [re stringByReplacingMatchesInString:s options:0
                                                   range:NSMakeRange(0, s.length)
                                            withTemplate:tmpl];
            }
        } else {
            if (r.find.length > 0) {
                s = [s stringByReplacingOccurrencesOfString:r.find withString:r.replace];
            } else if (s.length == 0) {
                s = r.replace;
            }
        }
    }
    return s;
}

@end

#pragma mark - Time

@implementation RMMeasureTime {
    NSString *_format;
    NSString *_cached;
}
- (BOOL)isStringMeasure { return YES; }
- (void)readSubclassOptions {
    _format = [self.parser readString:self.name key:@"Format" default:@"%H:%M:%S"];
}
// Windows strftime supports a '#' flag ("%#d") to strip leading zeros, which
// macOS/BSD strftime does not understand (it would emit the literal "#d").
// Map the day case to %e (blank-padded) and drop '#' elsewhere; the final
// string is whitespace-trimmed so a lone day renders as e.g. "5" not " 5".
static NSString *RMTranslateTimeFormat(NSString *fmt) {
    if (fmt.length == 0) return fmt ?: @"";
    NSString *s = fmt;
    s = [s stringByReplacingOccurrencesOfString:@"%#d" withString:@"%e"];
    s = [s stringByReplacingOccurrencesOfString:@"%#" withString:@"%"];
    return s;
}
- (void)updateValue {
    // strftime honours the current locale; %I/%M/%p/%a/%b/%d/%Y etc.
    time_t t = time(NULL);
    struct tm lt; localtime_r(&t, &lt);
    char buf[512];
    NSString *fmt = RMTranslateTimeFormat(_format);
    const char *cfmt = fmt.length ? fmt.UTF8String : "%H:%M:%S";
    if (strftime(buf, sizeof(buf), cfmt, &lt) > 0) {
        _cached = [[NSString stringWithUTF8String:buf]
                   stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    } else {
        _cached = @"";
    }
    self.value = (double)t;
}
- (nullable NSString *)rawString { return _cached ?: @""; }
@end

#pragma mark - Calc

@implementation RMMeasureCalc {
    NSString *_formula;
}
- (void)readSubclassOptions {
    _formula = [self.parser readString:self.name key:@"Formula" default:nil];
}
- (void)updateValue {
    if (_formula.length == 0) { self.value = 0; return; }
    // Re-expand every tick so #Var# / [Measure] changes take effect.
    NSString *expanded = [self.parser expand:_formula];
    double r = 0;
    RMConfigParser *cp = self.parser;
    RMMathVariableResolver resolver = ^BOOL(NSString *name, double *out) {
        if (cp.measureValueResolver) return cp.measureValueResolver(name, out);
        return NO;
    };
    if ([RMMathParser parse:expanded variableResolver:resolver result:&r]) self.value = r;
}
@end

#pragma mark - CPU

@implementation RMMeasureCPU {
    uint64_t _prevBusy, _prevTotal;
    BOOL _havePrev;
}
- (void)updateValue {
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &count) != KERN_SUCCESS) {
        return;
    }
    uint64_t user = info.cpu_ticks[CPU_STATE_USER];
    uint64_t sys  = info.cpu_ticks[CPU_STATE_SYSTEM];
    uint64_t nice = info.cpu_ticks[CPU_STATE_NICE];
    uint64_t idle = info.cpu_ticks[CPU_STATE_IDLE];
    uint64_t busy = user + sys + nice;
    uint64_t total = busy + idle;
    if (_havePrev && total > _prevTotal) {
        double dBusy = (double)(busy - _prevBusy);
        double dTotal = (double)(total - _prevTotal);
        self.value = dTotal > 0 ? (dBusy / dTotal * 100.0) : 0;
    } else {
        self.value = 0;
    }
    _prevBusy = busy; _prevTotal = total; _havePrev = YES;
    self.maxValue = 100;
}
@end

#pragma mark - Memory (Physical / Swap)

@implementation RMMeasureMemory
- (void)updateValue {
    if ([self.rawType isEqualToString:@"swapmemory"]) {
        struct xsw_usage sw;
        size_t len = sizeof(sw);
        if (sysctlbyname("vm.swapusage", &sw, &len, NULL, 0) == 0) {
            self.value = (double)sw.xsu_used;
            self.maxValue = (double)sw.xsu_total;
        }
        return;
    }
    // Physical memory: used = total - (free + inactive).
    uint64_t total = 0; size_t len = sizeof(total);
    sysctlbyname("hw.memsize", &total, &len, NULL, 0);

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        uint64_t freeBytes = ((uint64_t)vm.free_count + vm.inactive_count) * page;
        self.value = (double)(total > freeBytes ? total - freeBytes : 0);
    }
    self.maxValue = (double)total;
}
@end

#pragma mark - FreeDiskSpace

@implementation RMMeasureDisk {
    NSString *_path;
    BOOL _total;
}
- (void)readSubclassOptions {
    NSString *drive = [self.parser readString:self.name key:@"Drive" default:@"/"];
    // Windows drive letters have no meaning here; map C: (and any X:) to root.
    if ([drive hasSuffix:@":"] || drive.length == 0) _path = @"/";
    else _path = drive;
    _total = [self.parser readBool:self.name key:@"Total" default:NO];
}
- (void)updateValue {
    struct statfs st;
    if (statfs(_path.fileSystemRepresentation, &st) == 0) {
        double totalBytes = (double)st.f_blocks * st.f_bsize;
        double freeBytes  = (double)st.f_bavail * st.f_bsize;
        self.maxValue = totalBytes;
        self.value = _total ? totalBytes : freeBytes;
    }
}
@end

#pragma mark - Uptime

@implementation RMMeasureUptime {
    NSString *_format;
}
- (BOOL)isStringMeasure { return _format.length > 0; }
- (void)readSubclassOptions {
    _format = [self.parser readString:self.name key:@"Format" default:nil];
}
- (void)updateValue {
    struct timeval boot; size_t len = sizeof(boot);
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    double up = 0;
    if (sysctl(mib, 2, &boot, &len, NULL, 0) == 0) {
        up = (double)(time(NULL) - boot.tv_sec);
    }
    self.value = up;
}

// Substitute Rainmeter's printf-style Uptime format placeholders. Pattern:
//   %N!type! → Nth time component (1=total-seconds, 2=minutes, 3=hours, 4=days),
//   printed as the type 'i' (integer) with the optional suffix that follows.
// Anything else is left as a literal.
- (NSString *)formatUptimeWithTemplate:(NSString *)fmt {
    long s = (long)self.value;
    long sec = s % 60;
    long min = (s / 60) % 60;
    long hr  = (s / 3600) % 24;
    long day = s / 86400;
    NSMutableString *out = [NSMutableString string];
    NSUInteger i = 0, n = fmt.length;
    while (i < n) {
        unichar c = [fmt characterAtIndex:i];
        if (c != '%') { [out appendFormat:@"%C", c]; i++; continue; }
        // Parse %N!type! — N is digit, type is alphanumeric.
        if (i + 1 >= n) { [out appendFormat:@"%C", c]; i++; continue; }
        unichar nc = [fmt characterAtIndex:i + 1];
        if (nc < '0' || nc > '9') { [out appendFormat:@"%C", c]; i++; continue; }
        int nVal = nc - '0';
        NSUInteger j = i + 2;
        // Skip type specifier: alphanumeric until '!' or end.
        NSUInteger typeEnd = j;
        while (typeEnd < n) {
            unichar tc = [fmt characterAtIndex:typeEnd];
            if (tc == '!') break;
            if (!((tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z'))) break;
            typeEnd++;
        }
        if (typeEnd >= n) { [out appendFormat:@"%C", c]; i++; continue; }
        // typeEnd points at '!'. Read literal suffix after '!'.
        NSUInteger suffixEnd = typeEnd + 1;
        if (suffixEnd < n && [fmt characterAtIndex:suffixEnd] == '!') suffixEnd++;
        // Skip the value to use: nVal 1=sec, 2=min, 3=hr, 4=day.
        long comp = 0;
        switch (nVal) {
            case 1: comp = s; break;     // total seconds
            case 2: comp = min; break;
            case 3: comp = hr; break;
            case 4: comp = day; break;
            case 5: comp = sec; break;    // alias
            default: comp = 0; break;
        }
        [out appendFormat:@"%ld", comp];
        // Append any suffix that was not part of the %N!type! syntax.
        // (Rainmeter's %4!i!d %3!i!h leaves the "d" / "h" outside the placeholder.)
        if (suffixEnd < n && [fmt characterAtIndex:suffixEnd] != ' ') {
            // Heuristic: copy a single literal char as the suffix.
            [out appendFormat:@"%C", [fmt characterAtIndex:suffixEnd]];
            i = suffixEnd + 1;
        } else {
            i = suffixEnd;
        }
    }
    return out;
}

- (nullable NSString *)rawString {
    if (_format.length == 0) {
        // No custom format: default to "Xd Yh Zm Ws".
        long s = (long)self.value;
        long d = s / 86400; s %= 86400;
        long h = s / 3600;  s %= 3600;
        long m = s / 60;    long sec = s % 60;
        return [NSString stringWithFormat:@"%ldd %ldh %ldm %lds", d, h, m, sec];
    }
    return [self formatUptimeWithTemplate:_format];
}
@end

#pragma mark - Net

@implementation RMMeasureNet {
    uint64_t _prevIn, _prevOut;
    NSTimeInterval _prevT;
    BOOL _have;
}
- (void)updateValue {
    struct ifaddrs *ifs = NULL;
    uint64_t inB = 0, outB = 0;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *ifa = ifs; ifa; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_LINK) continue;
            if ((ifa->ifa_flags & IFF_LOOPBACK)) continue;
            struct if_data *d = (struct if_data *)ifa->ifa_data;
            if (d) { inB += d->ifi_ibytes; outB += d->ifi_obytes; }
        }
        freeifaddrs(ifs);
    }
    NSTimeInterval now = [NSDate date].timeIntervalSince1970;
    if (_have && now > _prevT) {
        double dt = now - _prevT;
        double dIn  = (double)(inB  - _prevIn)  / dt;
        double dOut = (double)(outB - _prevOut) / dt;
        if ([self.rawType isEqualToString:@"netin"]) self.value = dIn;
        else if ([self.rawType isEqualToString:@"netout"]) self.value = dOut;
        else self.value = dIn + dOut;
    } else {
        self.value = 0;
    }
    _prevIn = inB; _prevOut = outB; _prevT = now; _have = YES;
}
@end

#pragma mark - NowPlaying (real macOS implementation via AppleScript)

// Queries the active music player (Music.app, Spotify) via AppleScript, falling
// back to the stub idle state when no player is running or script execution fails.
@implementation RMMeasureNowPlaying {
    NSString *_playerType;
    NSString *_playerName; // "iTunes", "Spotify", "Music", or empty for auto-detect
    NSTimeInterval _lastQueryTime;
    NSMutableDictionary<NSString *, NSString *> *_cachedInfo; // playerType -> cached value
    BOOL _didInitialQuery;
}

- (BOOL)isStringMeasure { return YES; }

- (void)readSubclassOptions {
    _playerType = ([self.parser readString:self.name key:@"PlayerType" default:@"TITLE"]).uppercaseString;
    _playerName = [self.parser readString:self.name key:@"PlayerName" default:@""];
    self.maxValue = 1;
    _cachedInfo = [NSMutableDictionary dictionary];
    _didInitialQuery = NO;
}

- (void)updateValue {
    NSTimeInterval now = [NSDate date].timeIntervalSince1970;
    // Query at most once per second to avoid excessive AppleScript invocations.
    if (now - _lastQueryTime < 1.0 && _didInitialQuery) return;
    _lastQueryTime = now;
    _didInitialQuery = YES;

    // Auto-detect player or use specified one.
    NSString *player = _playerName;
    if (player.length == 0) {
        player = [self detectActivePlayer];
    }

    if (player.length == 0) {
        [self setIdleState];
        return;
    }

    NSDictionary *info = [self queryPlayer:player];
    if (info == nil || info.count == 0) {
        [self setIdleState];
        return;
    }

    [_cachedInfo addEntriesFromDictionary:info];
    NSString *state = _cachedInfo[@"STATE"] ?: @"0";

    if ([state isEqualToString:@"1"]) {
        self.value = 1.0;
    } else {
        self.value = 0.0;
    }

    // Parse duration/progress for numeric value.
    if ([_playerType isEqualToString:@"POSITION"]) {
        self.value = [self parseSeconds:_cachedInfo[@"POSITION"]];
        self.maxValue = [self parseSeconds:_cachedInfo[@"DURATION"]];
    } else if ([_playerType isEqualToString:@"DURATION"]) {
        self.value = [self parseSeconds:_cachedInfo[@"DURATION"]];
        self.maxValue = MAX(self.value, self.maxValue);
    } else if ([_playerType isEqualToString:@"PROGRESS"]) {
        double dur = [self parseSeconds:_cachedInfo[@"DURATION"]];
        double pos = [self parseSeconds:_cachedInfo[@"POSITION"]];
        self.value = dur > 0 ? (pos / dur * 100.0) : 0;
        self.maxValue = 100;
    }
}

- (nullable NSString *)rawString {
    NSString *val = _cachedInfo[_playerType];
    if (val) return val;

    // Return default idle values for known types.
    if ([_playerType isEqualToString:@"STATE"])       return @"0";
    if ([_playerType isEqualToString:@"POSITION"])    return @"0:00";
    if ([_playerType isEqualToString:@"DURATION"])    return @"0:00";
    if ([_playerType isEqualToString:@"PROGRESS"])    return @"0";
    return @"";
}

- (double)parseSeconds:(NSString *)timeStr {
    if (!timeStr || timeStr.length == 0) return 0;
    NSArray *parts = [timeStr componentsSeparatedByString:@":"];
    if (parts.count == 2) {
        return [parts[0] doubleValue] * 60 + [parts[1] doubleValue];
    }
    return [timeStr doubleValue];
}

- (void)setIdleState {
    [_cachedInfo setObject:@"0" forKey:@"STATE"];
    [_cachedInfo setObject:@"0:00" forKey:@"POSITION"];
    [_cachedInfo setObject:@"0:00" forKey:@"DURATION"];
    [_cachedInfo setObject:@"0" forKey:@"PROGRESS"];
    self.value = 0;
}

- (NSString *)detectActivePlayer {
    // Try Music.app (macOS 10.15+) first, then Spotify.
    for (NSString *app in @[@"Music", @"Spotify", @"iTunes"]) {
        if ([self isAppRunning:app]) return app;
    }
    return @"";
}

- (BOOL)isAppRunning:(NSString *)appName {
    NSArray *apps = [NSRunningApplication runningApplicationsWithBundleIdentifier:
                      [self bundleIDForApp:appName]];
    return apps.count > 0;
}

- (NSString *)bundleIDForApp:(NSString *)app {
    if ([app isEqualToString:@"Music"])  return @"com.apple.Music";
    if ([app isEqualToString:@"iTunes"]) return @"com.apple.iTunes";
    if ([app isEqualToString:@"Spotify"]) return @"com.spotify.client";
    return @"";
}

- (NSDictionary *)queryPlayer:(NSString *)player {
    NSString *script = nil;

    if ([player isEqualToString:@"Music"] || [player isEqualToString:@"iTunes"]) {
        script = [NSString stringWithFormat:
            @"tell application \"%@\"\n"
            @"  if player state is playing then\n"
            @"    set trackName to name of current track\n"
            @"    set trackArtist to artist of current track\n"
            @"    set trackAlbum to album of current track\n"
            @"    set trackDuration to duration of current track\n"
            @"    set trackPosition to player position\n"
            @"    set playerStateText to player state as text\n"
            @"    set trackData to trackName & \"|||\" & trackArtist & \"|||\" & trackAlbum & \"|||\" & trackDuration & \"|||\" & trackPosition & \"|||\" & playerStateText\n"
            @"    return trackData\n"
            @"  else\n"
            @"    return \"stopped\"\n"
            @"  end if\n"
            @"end tell", player];
    } else if ([player isEqualToString:@"Spotify"]) {
        script =
            @"tell application \"Spotify\"\n"
            @"  if player state is playing then\n"
            @"    set trackName to name of current track\n"
            @"    set trackArtist to artist of current track\n"
            @"    set trackAlbum to album of current track\n"
            @"    set trackDuration to duration of current track\n"
            @"    set trackPosition to player position\n"
            @"    set playerStateText to player state as text\n"
            @"    set trackData to trackName & \"|||\" & trackArtist & \"|||\" & trackAlbum & \"|||\" & trackDuration & \"|||\" & trackPosition & \"|||\" & playerStateText\n"
            @"    return trackData\n"
            @"  else\n"
            @"    return \"stopped\"\n"
            @"  end if\n"
            @"end tell";
    }

    if (script == nil) return nil;

    NSAppleScript *as = [[NSAppleScript alloc] initWithSource:script];
    NSDictionary *errorInfo = nil;
    NSAppleEventDescriptor *result = [as executeAndReturnError:&errorInfo];

    if (errorInfo || result == nil) return nil;

    NSString *text = [result stringValue];
    if (text == nil || [text isEqualToString:@"stopped"] || text.length == 0) {
        return @{@"STATE": @"0"};
    }

    NSArray *parts = [text componentsSeparatedByString:@"|||"];
    if (parts.count < 6) return @{@"STATE": @"0"};

    NSString *state = [parts[5] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    BOOL playing = [state caseInsensitiveCompare:@"playing"] == NSOrderedSame ||
                   [state caseInsensitiveCompare:@"kPSP"] == NSOrderedSame;

    double duration = [parts[3] doubleValue] / 1000.0; // milliseconds to seconds
    double position = [parts[4] doubleValue] / 1000.0;

    NSString *durStr = [self formatTime:duration];
    NSString *posStr = [self formatTime:position];

    return @{
        @"TITLE":    [parts[0] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]],
        @"ARTIST":   [parts[1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]],
        @"ALBUM":    [parts[2] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]],
        @"DURATION": durStr,
        @"POSITION": posStr,
        @"PROGRESS": [NSString stringWithFormat:@"%.1f", duration > 0 ? position / duration * 100 : 0],
        @"STATE":    playing ? @"1" : @"0",
    };
}

- (NSString *)formatTime:(double)seconds {
    if (seconds < 0) seconds = 0;
    long m = (long)(seconds / 60);
    long s = (long)(seconds) % 60;
    return [NSString stringWithFormat:@"%ld:%02ld", m, s];
}

@end

#pragma mark - Battery / Power

@implementation RMMeasureBattery {
    BOOL _isString;
    NSString *_type; // percent / lifetime / status / acline / mhz
}
- (BOOL)isStringMeasure { return _isString; }
- (void)readSubclassOptions {
    NSString *ps = [self.parser readString:self.name key:@"PowerState" default:@"PERCENT"];
    _type = ps.uppercaseString;
    _isString = [_type isEqualToString:@"STATUS"] || [_type isEqualToString:@"ACLINE"];
    self.maxValue = 100;
}
- (void)updateValue {
    // Use IOKit to read battery info. Fall back to 100% if no battery.
    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    CFArrayRef list = IOPSCopyPowerSourcesList(blob);
    double pct = 100, lifetime = -1;
    BOOL charging = NO, present = NO;
    if (list && CFArrayGetCount(list) > 0) {
        for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
            CFTypeRef ps = CFArrayGetValueAtIndex(list, i);
            CFDictionaryRef dict = IOPSGetPowerSourceDescription(blob, ps);
            if (dict == NULL) continue;
            present = YES;
            CFNumberRef cur = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("Current Capacity"));
            CFNumberRef max = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("Max Capacity"));
            if (cur && max) {
                int c = 0, m = 0;
                CFNumberGetValue(cur, kCFNumberIntType, &c);
                CFNumberGetValue(max, kCFNumberIntType, &m);
                if (m > 0) pct = (double)c / (double)m * 100.0;
            }
            CFBooleanRef ac = (CFBooleanRef)CFDictionaryGetValue(dict, CFSTR("Is Charging"));
            if (ac) charging = CFBooleanGetValue(ac);
            CFNumberRef time = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("Time to Empty"));
            if (time) CFNumberGetValue(time, kCFNumberIntType, &lifetime);
            if (!charging && lifetime < 0) {
                CFNumberRef full = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("Time to Full Charge"));
                if (full) CFNumberGetValue(full, kCFNumberIntType, &lifetime);
            }
        }
    }
    if (list) CFRelease(list);
    CFRelease(blob);
    if (!present) { self.value = 100; self.maxValue = 100; return; }
    self.value = pct;
    self.maxValue = 100;
}
- (nullable NSString *)rawString {
    if (!_isString) return nil;
    if ([_type isEqualToString:@"STATUS"]) {
        if (self.value >= 99) return @"Full";
        if (self.value > 20)  return @"Discharging";
        return @"Low";
    }
    if ([_type isEqualToString:@"ACLINE"]) {
        return self.value >= 99 ? @"AC Line" : @"Battery";
    }
    return nil;
}
@end

#pragma mark - SysInfo

@implementation RMMeasureSysInfo {
    BOOL _isString;
    NSString *_dataType; // HOST_NAME, USER_NAME, OS_VERSION, OS_BITS, COMPUTER_NAME, etc.
}
- (BOOL)isStringMeasure { return _isString; }
- (void)readSubclassOptions {
    _dataType = [[self.parser readString:self.name key:@"SysInfoType" default:@"HOST_NAME"] uppercaseString];
    _isString = YES;
}
- (void)updateValue { self.value = 0; }
- (nullable NSString *)rawString {
    if ([_dataType isEqualToString:@"HOST_NAME"])       return NSProcessInfo.processInfo.hostName ?: @"";
    if ([_dataType isEqualToString:@"USER_NAME"] ||
        [_dataType isEqualToString:@"COMPUTER_NAME"])   return NSFullUserName();
    if ([_dataType isEqualToString:@"OS_VERSION"])      return NSProcessInfo.processInfo.operatingSystemVersionString ?: @"";
    if ([_dataType isEqualToString:@"OS_BITS"])         return @"64";
    if ([_dataType isEqualToString:@"OS_ARCHITECTURE"]) return @"arm64";
    if ([_dataType isEqualToString:@"DATE"])            return [NSDateFormatter localizedStringFromDate:[NSDate date] dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterNoStyle];
    if ([_dataType isEqualToString:@"TIME"])            return [NSDateFormatter localizedStringFromDate:[NSDate date] dateStyle:NSDateFormatterNoStyle timeStyle:NSDateFormatterShortStyle];
    return @"";
}
@end

#pragma mark - Process (top CPU/memory process)

@implementation RMMeasureProcess {
    NSString *_processName;
    NSString *_counter; // % Processor Time, Working Set - Private, ID Process
    int _processId;
    BOOL _topProcess; // if true, find the top process; otherwise use ProcessName
}
- (BOOL)isStringMeasure { return _processName.length > 0; }
- (void)readSubclassOptions {
    _processName = [self.parser readString:self.name key:@"ProcessName" default:nil];
    _processId   = [self.parser readInt:self.name key:@"ProcessID" default:-1];
    _counter     = ([self.parser readString:self.name key:@"Counter" default:@"% Processor Time"]).uppercaseString;
    _topProcess  = [self.parser readBool:self.name key:@"TopProcess" default:NO];
    self.maxValue = 100;
}
- (void)updateValue {
    // Top process discovery via NSTask: use `ps` to find the highest-CPU process.
    // This is a best-effort approximation on macOS.
    if (_topProcess) {
        // Use `ps -eo pid,pcpu,rss,comm -r` sorted by CPU.
        NSTask *task = [[NSTask alloc] init];
        task.launchPath = @"/bin/ps";
        task.arguments = @[@"-eo", @"pid,pcpu,rss,comm", @"-r"];
        NSPipe *pipe = [NSPipe pipe];
        task.standardOutput = pipe;
        task.standardError = [NSFileHandle fileHandleWithNullDevice];
        @try {
            [task launch];
            [task waitUntilExit];
            NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
            NSString *output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            NSArray *lines = [output componentsSeparatedByString:@"\n"];
            if (lines.count >= 2) {
                // First non-header line (index 1) is the top process.
                NSString *topLine = lines[1];
                // Parse: pid pcpu rss comm
                NSScanner *sc = [NSScanner scannerWithString:topLine];
                int pid = 0; double cpu = 0; long long rss = 0;
                [sc scanInt:&pid];
                [sc scanDouble:&cpu];
                [sc scanLongLong:&rss];
                NSString *comm = [topLine substringFromIndex:sc.scanLocation];
                comm = [comm stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
                if ([_counter containsString:@"PROCESSOR"]) {
                    self.value = cpu;
                } else if ([_counter containsString:@"WORKING"] || [_counter containsString:@"MEMORY"]) {
                    self.value = (double)rss;
                    self.maxValue = (double)NSProcessInfo.processInfo.physicalMemory;
                } else {
                    self.value = cpu;
                }
                self.maxValue = 100;
                _processName = comm;
            }
        } @catch (NSException *e) { self.value = 0; }
    } else {
        self.value = 0;
    }
}
- (nullable NSString *)rawString {
    return _processName ?: @"";
}
@end

#pragma mark - WebParser (NSURLSession + regex capture)

@interface RMMeasureWebParser () <NSURLSessionDataDelegate> {
    NSString *_url;
    NSString *_regexp;
    int _updateRate;
    BOOL _debug;
    NSString *_downloadFile;
    NSString *_download;
    NSString *_finishAction;
    NSMutableDictionary<NSNumber *, NSString *> *_stringIndexes; // index -> capture
    NSMutableDictionary<NSNumber *, NSString *> *_childMeasures;  // ChildName -> parent measure name
    NSMutableArray<NSDictionary *> *_matchResults;  // array of dicts {index: value}
    NSURLSession *_session;
    NSURLSessionDataTask *_task;
    NSMutableData *_responseData;
    BOOL _hasValidData;
    BOOL _parsing;
    NSString *_errorString;
    NSInteger _httpCode;
    int _ticksSinceLastFetch;
}
@end

@implementation RMMeasureWebParser

- (BOOL)isStringMeasure { return YES; }

- (void)readSubclassOptions {
    RMConfigParser *cp = self.parser;
    _url = [cp readString:self.name key:@"URL" default:nil];
    _regexp = [cp readString:self.name key:@"RegExp" default:nil];
    _updateRate = [cp readInt:self.name key:@"UpdateRate" default:600]; // 10 min default like Rainmeter
    if (_updateRate < 1) _updateRate = 1;
    _debug = [cp readBool:self.name key:@"Debug" default:NO];
    _download = [cp readBool:self.name key:@"Download" default:NO] ? @"1" : nil;
    _downloadFile = [cp readString:self.name key:@"DownloadFile" default:nil];
    _finishAction = [cp readString:self.name key:@"FinishAction" default:nil];

    // Collect StringIndexN entries for capture groups.
    _stringIndexes = [NSMutableDictionary dictionary];
    RMIniSection *sec = [cp.ini sectionNamed:self.name];
    for (int i = 1; i <= 99; i++) {
        NSString *key = (i == 1) ? @"StringIndex" : [NSString stringWithFormat:@"StringIndex%d", i];
        NSString *val = [sec valueForKey:key];
        if (val.length == 0) { if (i > 1) break; else continue; }
        _stringIndexes[@(i)] = val;
    }

    // Child measures: other WebParser measures in the same skin whose URL is this measure's name
    // in brackets, e.g. URL=[ParentMeasure]. They auto-inherit the regex matches from the parent.
    _childMeasures = [NSMutableDictionary dictionary];

    // Read RegExpSubstitute/Substitute from parent class via readConditionOptions
    _hasValidData = NO;
    _parsing = NO;
    _errorString = nil;
    _httpCode = 0;
    _ticksSinceLastFetch = _updateRate; // Fetch immediately on first tick
    _matchResults = [NSMutableArray array];
}

// Called by RMSkin after all measures are created to link child WebParser measures.
- (void)registerChildMeasureNamed:(NSString *)childName {
    _childMeasures[childName.uppercaseString] = childName;
}

- (void)updateValue {
    self.value = _hasValidData ? 1.0 : 0.0;

    _ticksSinceLastFetch++;
    if (_ticksSinceLastFetch >= _updateRate) {
        _ticksSinceLastFetch = 0;
        [self startFetch];
    }
}

- (nullable NSString *)rawString {
    // Return the first capture group (StringIndex=1) by default.
    NSDictionary *firstMatch = _matchResults.firstObject;
    if (firstMatch) {
        NSString *s = firstMatch[@(1)];
        if (s) return s;
    }
    // Fallback: return entire response body if no regex captures.
    if (_responseData && _hasValidData) {
        return [[NSString alloc] initWithData:_responseData encoding:NSUTF8StringEncoding] ?: @"";
    }
    if (_errorString) return _errorString;
    return @"";
}

// Resolve StringIndex N from the match results, used by child measures.
- (nullable NSString *)stringForIndex:(int)index {
    NSDictionary *firstMatch = _matchResults.firstObject;
    return firstMatch ? firstMatch[@(index)] : nil;
}

- (BOOL)hasValidData { return _hasValidData; }
- (nullable NSString *)errorString { return _errorString; }
- (NSInteger)httpCode { return _httpCode; }
- (nullable NSString *)downloadFile { return _downloadFile; }
- (nullable NSString *)finishAction { return _finishAction; }

- (void)startFetch {
    if (_url.length == 0) return;
    if (_parsing) return; // Already fetching

    NSString *expandedURL = [self.parser expand:_url];
    NSURL *nsurl = [NSURL URLWithString:expandedURL];
    if (!nsurl) {
        _errorString = [NSString stringWithFormat:@"Invalid URL: %@", expandedURL];
        return;
    }

    _parsing = YES;
    _responseData = [NSMutableData data];
    _errorString = nil;
    _httpCode = 0;

    // Use a simple synchronous fetch via dispatch_semaphore for simplicity.
    // Rainmeter's WebParser uses blocking sockets on worker threads; we do the
    // same idiomatically via NSURLSession with a completion block.
    NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.timeoutIntervalForRequest = 30;
    config.timeoutIntervalForResource = 60;
    _session = [NSURLSession sessionWithConfiguration:config
                                             delegate:nil
                                        delegateQueue:[NSOperationQueue new]];

    __weak RMMeasureWebParser *weakSelf = self;
    _task = [_session dataTaskWithURL:nsurl completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        RMMeasureWebParser *strongSelf = weakSelf;
        if (!strongSelf) return;

        if (error) {
            strongSelf->_errorString = error.localizedDescription;
            strongSelf->_parsing = NO;
            return;
        }

        NSHTTPURLResponse *httpResp = nil;
        if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
            httpResp = (NSHTTPURLResponse *)response;
            strongSelf->_httpCode = httpResp.statusCode;
        }

        strongSelf->_responseData = [data mutableCopy] ?: [NSMutableData data];
        strongSelf->_hasValidData = YES;

        if (strongSelf->_download.length && strongSelf->_downloadFile.length) {
            [strongSelf saveDownloadedFile];
        }

        if (strongSelf->_regexp.length) {
            [strongSelf parseWithRegex];
        }

        strongSelf->_parsing = NO;

        // Fire FinishAction if present.
        if (strongSelf->_finishAction.length && strongSelf.executeAction) {
            NSString *wrapped = ([strongSelf->_finishAction hasPrefix:@"["] ? strongSelf->_finishAction
                                  : [NSString stringWithFormat:@"[%@]", strongSelf->_finishAction]);
            strongSelf.executeAction(wrapped);
        }
    }];
    [_task resume];
}

- (void)saveDownloadedFile {
    NSString *path = [self.parser expand:_downloadFile];
    path = [path stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
    if (!path.isAbsolutePath) {
        path = [self.parser.currentPath stringByAppendingPathComponent:path];
    }
    NSString *dir = [path stringByDeletingLastPathComponent];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES
                                               attributes:nil error:nil];
    NSError *err = nil;
    [_responseData writeToFile:path options:NSDataWritingAtomic error:&err];
    if (err) {
        RMLogWarn(@"WebParser download failed: %@ -> %@, error: %@", _url, path, err);
    } else if (_debug) {
        RMLogDebug(@"WebParser downloaded %lu bytes to %@", (unsigned long)_responseData.length, path);
    }
}

- (void)parseWithRegex {
    _matchResults = [NSMutableArray array];
    NSString *body = [[NSString alloc] initWithData:_responseData encoding:NSUTF8StringEncoding];
    if (!body && _responseData.length > 0) {
        body = [[NSString alloc] initWithData:_responseData encoding:NSISOLatin1StringEncoding];
    }
    if (!body) return;

    NSError *err = nil;
    NSString *pattern = [self.parser expand:_regexp];
    NSRegularExpressionOptions opts = NSRegularExpressionDotMatchesLineSeparators;
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:pattern
                                                                        options:opts
                                                                          error:&err];
    if (err) {
        RMLogWarn(@"WebParser RegExp error: %@", err);
        _errorString = [NSString stringWithFormat:@"RegExp error: %@", err.localizedDescription];
        return;
    }

    NSArray<NSTextCheckingResult *> *matches = [re matchesInString:body
                                                            options:0
                                                              range:NSMakeRange(0, body.length)];

    for (NSTextCheckingResult *match in matches) {
        NSMutableDictionary *entry = [NSMutableDictionary dictionary];
        for (NSUInteger i = 0; i <= match.numberOfRanges && i <= 99; i++) {
            NSRange r = (i < match.numberOfRanges) ? [match rangeAtIndex:i] : NSMakeRange(NSNotFound, 0);
            if (r.location != NSNotFound) {
                entry[@(i)] = [body substringWithRange:r];
            } else {
                entry[@(i)] = @"";
            }
        }
        [_matchResults addObject:entry];
    }

    // Also capture full body as match index 0
    if (_matchResults.count == 0) {
        NSMutableDictionary *entry = [NSMutableDictionary dictionary];
        entry[@(0)] = body;
        [_matchResults addObject:entry];
    }

    if (_debug) {
        RMLogDebug(@"WebParser: %lu matches for '%@'", (unsigned long)_matchResults.count, _url);
    }
}

@end

#pragma mark - RunCommand (NSTask shell execution)

@implementation RMMeasureRunCommand {
    NSString *_program;
    NSString *_parameter;
    NSString *_outputType;
    NSString *_state;
    int _timeout;
    NSString *_finishAction;
    NSString *_lastOutput;
    double _lastExitCode;
    BOOL _running;
}

- (BOOL)isStringMeasure { return YES; }

- (void)readSubclassOptions {
    RMConfigParser *cp = self.parser;
    _program = [cp readString:self.name key:@"Program" default:nil];
    _parameter = [cp readString:self.name key:@"Parameter" default:@""];
    _outputType = ([cp readString:self.name key:@"OutputType" default:@"UTF8"]).uppercaseString;
    _state = ([cp readString:self.name key:@"State" default:@"Hide"]).uppercaseString;
    _timeout = [cp readInt:self.name key:@"Timeout" default:0];
    _finishAction = [cp readString:self.name key:@"FinishAction" default:nil];
    _lastOutput = @"";
    _lastExitCode = -1;
    _running = NO;
}

- (void)updateValue {
    // Launch only once; subsequent ticks just return the cached result.
    // The command result stays valid until the skin is refreshed.
    if (_running) return;
    if (_lastExitCode >= 0) return; // already ran
    if (_program.length == 0) return;
    [self launchCommand];
}

- (void)launchCommand {
    _running = YES;
    NSString *program = [self.parser expand:_program];
    NSString *parameter = [self.parser expand:_parameter];

    __weak RMMeasureRunCommand *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        RMMeasureRunCommand *strongSelf = weakSelf;
        if (!strongSelf) return;

        NSTask *task = [[NSTask alloc] init];

        // Resolve the program path. If it's just a name (e.g. "python3"), look up via /usr/bin/env.
        NSString *launchPath = program;
        if (![program hasPrefix:@"/"] && ![program hasPrefix:@"."]) {
            NSTask *which = [[NSTask alloc] init];
            which.launchPath = @"/usr/bin/env";
            which.arguments = @[@"which", program];
            NSPipe *pipe = [NSPipe pipe];
            which.standardOutput = pipe;
            which.standardError = [NSFileHandle fileHandleWithNullDevice];
            @try {
                [which launch];
                [which waitUntilExit];
                NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
                NSString *found = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
                found = [found stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
                if (found.length > 0 && which.terminationStatus == 0) {
                    launchPath = found;
                }
            } @catch (NSException *e) {
                // fall through to try `program` as-is
            }
        }

        task.launchPath = launchPath;

        // Parse parameters: split by space, respecting quotes.
        if (parameter.length > 0) {
            NSMutableArray *args = [NSMutableArray array];
            [self tokenizeArguments:parameter into:args];
            task.arguments = args;
        } else {
            task.arguments = @[];
        }

        NSPipe *outPipe = [NSPipe pipe];
        NSPipe *errPipe = [NSPipe pipe];
        task.standardOutput = outPipe;
        task.standardError = errPipe;

        @try {
            [task launch];

            if (strongSelf->_timeout > 0) {
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(strongSelf->_timeout * NSEC_PER_SEC)),
                               dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                    if (task.isRunning) [task terminate];
                });
            }

            [task waitUntilExit];

            NSData *outData = [[outPipe fileHandleForReading] readDataToEndOfFile];
            NSData *errData = [[errPipe fileHandleForReading] readDataToEndOfFile];

            NSString *output = @"";
            if (outData.length > 0) {
                NSStringEncoding enc = [strongSelf->_outputType isEqualToString:@"ANSI"] ?
                    NSISOLatin1StringEncoding : NSUTF8StringEncoding;
                output = [[NSString alloc] initWithData:outData encoding:enc] ?: @"";
            }
            if (errData.length > 0 && output.length == 0) {
                output = [[NSString alloc] initWithData:errData encoding:NSUTF8StringEncoding] ?: @"";
            }

            strongSelf->_lastOutput = [output stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
            strongSelf->_lastExitCode = (double)task.terminationStatus;
            strongSelf.value = strongSelf->_lastExitCode;

        } @catch (NSException *e) {
            strongSelf->_lastOutput = @"";
            strongSelf->_lastExitCode = -1;
            RMLogWarn(@"RunCommand failed: %@", e);
        }

        strongSelf->_running = NO;

        if (strongSelf->_finishAction.length && strongSelf.executeAction) {
            dispatch_async(dispatch_get_main_queue(), ^{
                NSString *wrapped = ([strongSelf->_finishAction hasPrefix:@"["] ? strongSelf->_finishAction
                                      : [NSString stringWithFormat:@"[%@]", strongSelf->_finishAction]);
                strongSelf.executeAction(wrapped);
            });
        }
    });
}

- (void)tokenizeArguments:(NSString *)str into:(NSMutableArray<NSString *> *)args {
    NSUInteger i = 0, n = str.length;
    NSMutableString *cur = [NSMutableString string];
    BOOL inQuote = NO; unichar q = '"';
    while (i < n) {
        unichar c = [str characterAtIndex:i];
        if (inQuote) {
            if (c == q) { inQuote = NO; }
            else [cur appendFormat:@"%C", c];
        } else {
            if (c == '"' || c == '\'') { inQuote = YES; q = c; }
            else if (c == ' ' || c == '\t') {
                if (cur.length > 0) { [args addObject:cur.copy]; [cur setString:@""]; }
            } else {
                [cur appendFormat:@"%C", c];
            }
        }
        i++;
    }
    if (cur.length > 0) [args addObject:cur.copy];
}

- (nullable NSString *)rawString {
    return _lastOutput ?: @"";
}

@end

#pragma mark - FolderInfo

@implementation RMMeasureFolderInfo {
    NSString *_folder;
    BOOL _includeSubFolders;
    NSString *_type; // FileCount, FolderCount, Size
}

- (void)readSubclassOptions {
    RMConfigParser *cp = self.parser;
    _folder = [cp readString:self.name key:@"Folder" default:cp.currentPath];
    _folder = [cp expand:_folder];
    _folder = [_folder stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
    _includeSubFolders = [cp readBool:self.name key:@"IncludeSubFolders" default:NO];
    _type = ([cp readString:self.name key:@"Type" default:@"FileCount"]).uppercaseString;
}

- (void)updateValue {
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:_folder isDirectory:&isDir]) {
        self.value = 0;
        return;
    }

    if ([_type isEqualToString:@"FOLDERCOUNT"]) {
        if (!isDir) { self.value = 0; return; }
        self.value = [self countItems:_folder ofType:YES manager:fm recursive:_includeSubFolders];
    } else if ([_type isEqualToString:@"FILECOUNT"]) {
        if (!isDir) { self.value = 1; return; }
        self.value = [self countItems:_folder ofType:NO manager:fm recursive:_includeSubFolders];
    } else if ([_type isEqualToString:@"SIZE"]) {
        if (!isDir) {
            NSDictionary *attrs = [fm attributesOfItemAtPath:_folder error:nil];
            self.value = (double)[attrs[NSFileSize] unsignedLongLongValue];
            return;
        }
        self.value = [self calcSize:_folder manager:fm recursive:_includeSubFolders];
    }
    self.maxValue = MAX(self.value, self.maxValue);
}

- (NSUInteger)countItems:(NSString *)path ofType:(BOOL)folders
                manager:(NSFileManager *)fm recursive:(BOOL)recursive {
    NSUInteger count = 0;
    NSArray *items = [fm contentsOfDirectoryAtPath:path error:nil];
    for (NSString *item in items) {
        NSString *full = [path stringByAppendingPathComponent:item];
        BOOL isDir = NO;
        [fm fileExistsAtPath:full isDirectory:&isDir];
        if (isDir == folders) count++;
        if (isDir && recursive) {
            count += [self countItems:full ofType:folders manager:fm recursive:YES];
        }
    }
    return count;
}

- (double)calcSize:(NSString *)path manager:(NSFileManager *)fm recursive:(BOOL)recursive {
    double total = 0;
    NSArray *items = [fm contentsOfDirectoryAtPath:path error:nil];
    for (NSString *item in items) {
        NSString *full = [path stringByAppendingPathComponent:item];
        BOOL isDir = NO;
        [fm fileExistsAtPath:full isDirectory:&isDir];
        if (isDir) {
            if (recursive) total += [self calcSize:full manager:fm recursive:YES];
        } else {
            NSDictionary *attrs = [fm attributesOfItemAtPath:full error:nil];
            total += (double)[attrs[NSFileSize] unsignedLongLongValue];
        }
    }
    return total;
}

@end

#pragma mark - Registry (macOS: NSUserDefaults / plist)

@implementation RMMeasureRegistry {
    NSString *_regKey;
    NSString *_regValue;
    BOOL _isString;
}

- (BOOL)isStringMeasure { return _isString; }

- (void)readSubclassOptions {
    RMConfigParser *cp = self.parser;
    _regKey = [cp readString:self.name key:@"RegKey" default:nil];
    _regValue = [cp readString:self.name key:@"RegValue" default:nil];
    // If the key looks like a path to a plist file, read from that;
    // otherwise use NSUserDefaults (Rainmeter's registry maps to macOS defaults).
    _isString = YES;
}

- (void)updateValue {
    self.value = 0;
    if (_regKey.length == 0 || _regValue.length == 0) return;

    // Try plist file first, then NSUserDefaults.
    NSString *expandedKey = [self.parser expand:_regKey];
    NSString *expandedValue = [self.parser expand:_regValue];

    // Rainmeter HKEY_CURRENT_USER\Software\... maps to NSUserDefaults domain.
    // We support direct plist paths too.
    NSString *keyNorm = [expandedKey stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];

    if (keyNorm.isAbsolutePath && [[NSFileManager defaultManager] fileExistsAtPath:keyNorm]) {
        NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:keyNorm];
        id val = plist[expandedValue];
        if ([val isKindOfClass:[NSNumber class]]) {
            self.value = [val doubleValue];
        } else if ([val isKindOfClass:[NSString class]]) {
            double d = 0;
            if ([[NSScanner scannerWithString:val] scanDouble:&d]) self.value = d;
        }
    } else {
        // Use CFPreferences for the given domain (macOS equivalent of HKCU registry).
        NSString *domain = (keyNorm.length > 0) ? keyNorm : (NSString *)kCFPreferencesCurrentApplication;
        CFPropertyListRef val = CFPreferencesCopyValue((__bridge CFStringRef)expandedValue,
                                                        (__bridge CFStringRef)domain,
                                                        kCFPreferencesCurrentUser,
                                                        kCFPreferencesAnyHost);
        if (val) {
            if (CFGetTypeID(val) == CFNumberGetTypeID()) {
                { double tmp = 0; CFNumberGetValue((CFNumberRef)val, kCFNumberDoubleType, &tmp); self.value = tmp; }
            } else if (CFGetTypeID(val) == CFStringGetTypeID()) {
                double d = 0;
                if ([[NSScanner scannerWithString:(__bridge NSString *)val] scanDouble:&d]) self.value = d;
            } else if (CFGetTypeID(val) == CFBooleanGetTypeID()) {
                self.value = CFBooleanGetValue((CFBooleanRef)val) ? 1.0 : 0.0;
            }
            CFRelease(val);
        }
    }
}

- (nullable NSString *)rawString {
    if (!_isString) return nil;
    NSString *expandedKey = [self.parser expand:_regKey];
    NSString *expandedValue = [self.parser expand:_regValue];
    if (expandedKey.length == 0 || expandedValue.length == 0) return @"";

    NSString *keyNorm = [expandedKey stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
    if (keyNorm.isAbsolutePath && [[NSFileManager defaultManager] fileExistsAtPath:keyNorm]) {
        NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:keyNorm];
        id val = plist[expandedValue];
        if ([val isKindOfClass:[NSString class]]) return val;
        if ([val isKindOfClass:[NSNumber class]]) return [val stringValue];
        return @"";
    }

    NSString *domain = (keyNorm.length > 0) ? keyNorm : (NSString *)kCFPreferencesCurrentApplication;
    CFPropertyListRef val = CFPreferencesCopyValue((__bridge CFStringRef)expandedValue,
                                                    (__bridge CFStringRef)domain,
                                                    kCFPreferencesCurrentUser,
                                                    kCFPreferencesAnyHost);
    if (val) {
        NSString *result = @"";
        if (CFGetTypeID(val) == CFStringGetTypeID()) {
            result = (__bridge NSString *)val;
        } else if (CFGetTypeID(val) == CFNumberGetTypeID()) {
            result = [(__bridge NSNumber *)val stringValue];
        } else if (CFGetTypeID(val) == CFBooleanGetTypeID()) {
            result = CFBooleanGetValue((CFBooleanRef)val) ? @"1" : @"0";
        }
        CFRelease(val);
        return result;
    }
    return @"";
}

@end

#pragma mark - RecycleManager (macOS: ~/.Trash)

@implementation RMMeasureRecycleManager {
    NSString *_type; // Count, Size
    NSString *_drive; // Ignored on macOS, single trash
}

- (void)readSubclassOptions {
    RMConfigParser *cp = self.parser;
    _type = ([cp readString:self.name key:@"Type" default:@"Count"]).uppercaseString;
    _drive = [cp readString:self.name key:@"Drives" default:nil];
}

- (void)updateValue {
    NSString *trashPath = [NSHomeDirectory() stringByAppendingPathComponent:@".Trash"];
    NSFileManager *fm = [NSFileManager defaultManager];

    if ([_type isEqualToString:@"SIZE"]) {
        double total = 0;
        NSArray *items = [fm contentsOfDirectoryAtPath:trashPath error:nil];
        for (NSString *item in items) {
            NSString *full = [trashPath stringByAppendingPathComponent:item];
            NSDictionary *attrs = [fm attributesOfItemAtPath:full error:nil];
            total += (double)[attrs[NSFileSize] unsignedLongLongValue];
        }
        self.value = total;
        self.maxValue = MAX(total, self.maxValue);
    } else {
        // Count (default)
        NSArray *items = [fm contentsOfDirectoryAtPath:trashPath error:nil];
        // Filter out .DS_Store
        NSUInteger count = 0;
        for (NSString *item in items) {
            if (![item hasPrefix:@"."]) count++;
        }
        self.value = (double)count;
        self.maxValue = MAX((double)count, self.maxValue);
    }
}

@end

#pragma mark - WiFiStatus (CoreWLAN)

#import <CoreWLAN/CoreWLAN.h>

@implementation RMMeasureWiFiStatus {
    NSString *_infoType; // SSID, Quality, Encryption, Description, PHY, Authenticated,
                          // Channel, IPAddress, SubnetMask, Gateway, DNS
    BOOL _isString;
}

- (BOOL)isStringMeasure { return _isString; }

- (void)readSubclassOptions {
    RMConfigParser *cp = self.parser;
    _infoType = ([cp readString:self.name key:@"WiFiInfoType" default:@"SSID"]).uppercaseString;
    _isString = ![@[@"QUALITY", @"CHANNEL"] containsObject:_infoType];
}

- (void)updateValue {
    self.value = 0;
    CWInterface *wifi = [[CWWiFiClient sharedWiFiClient] interface];
    if (!wifi) return;

    if ([_infoType isEqualToString:@"SSID"]) {
        self.value = wifi.ssid.length > 0 ? 1 : 0;
    } else if ([_infoType isEqualToString:@"QUALITY"]) {
        // RSSI mapped to 0-100 scale. Typical RSSI: -30 (excellent) to -90 (poor).
        double rssi = wifi.rssiValue;
        if (rssi < 0) {
            // Map -90..-30 to 0..100
            self.value = MAX(0, MIN(100, (rssi + 90) / 60.0 * 100.0));
        }
    } else if ([_infoType isEqualToString:@"ENCRYPTION"]) {
        self.value = wifi.security != kCWSecurityNone ? 1 : 0;
    } else if ([_infoType isEqualToString:@"AUTHENTICATED"]) {
        self.value = (wifi.ssid.length > 0) ? 1 : 0;
    } else if ([_infoType isEqualToString:@"CHANNEL"]) {
        CWChannel *ch = wifi.wlanChannel;
        self.value = (double)ch.channelNumber;
    } else if ([_infoType isEqualToString:@"PHY"]) {
        self.value = 1; // Always have some PHY if connected
    } else if ([_infoType isEqualToString:@"IPADDRESS"] ||
               [_infoType isEqualToString:@"SUBNETMASK"] ||
               [_infoType isEqualToString:@"GATEWAY"] ||
               [_infoType isEqualToString:@"DNS"]) {
        self.value = [self resolveIPField] ? 1 : 0;
    } else {
        self.value = 0;
    }
    self.maxValue = MAX(self.value, 100);
}

- (nullable NSString *)rawString {
    if (!_isString) return nil;
    CWInterface *wifi = [[CWWiFiClient sharedWiFiClient] interface];
    if (!wifi) return @"";

    if ([_infoType isEqualToString:@"SSID"]) return wifi.ssid ?: @"";
    if ([_infoType isEqualToString:@"ENCRYPTION"] || [_infoType isEqualToString:@"DESCRIPTION"]) {
        switch (wifi.security) {
            case kCWSecurityNone: return @"None";
            case kCWSecurityWEP: return @"WEP";
            case kCWSecurityWPAPersonal: return @"WPA-Personal";
            case kCWSecurityWPAPersonalMixed: return @"WPA-Personal-Mixed";
            case kCWSecurityWPA2Personal: return @"WPA2-Personal";
            case kCWSecurityPersonal: return @"Personal";
            case kCWSecurityDynamicWEP: return @"Dynamic WEP";
            case kCWSecurityWPAEnterprise: return @"WPA-Enterprise";
            case kCWSecurityWPAEnterpriseMixed: return @"WPA-Enterprise-Mixed";
            case kCWSecurityWPA2Enterprise: return @"WPA2-Enterprise";
            case kCWSecurityEnterprise: return @"Enterprise";
            case kCWSecurityWPA3Personal: return @"WPA3-Personal";
            case kCWSecurityWPA3Enterprise: return @"WPA3-Enterprise";
            case kCWSecurityWPA3Transition: return @"WPA3-Transition";
            default: return @"Unknown";
        }
    }
    if ([_infoType isEqualToString:@"PHY"]) {
        switch (wifi.activePHYMode) {
            case kCWPHYMode11a: return @"802.11a";
            case kCWPHYMode11b: return @"802.11b";
            case kCWPHYMode11g: return @"802.11g";
            case kCWPHYMode11n: return @"802.11n";
            case kCWPHYMode11ac: return @"802.11ac";
            case kCWPHYMode11ax: return @"802.11ax";
            default: return @"Unknown";
        }
    }
    if ([_infoType isEqualToString:@"AUTHENTICATED"]) {
        return wifi.ssid.length > 0 ? @"1" : @"0";
    }
    if ([_infoType isEqualToString:@"CHANNEL"]) {
        CWChannel *ch = wifi.wlanChannel;
        return [NSString stringWithFormat:@"%ld", (long)ch.channelNumber];
    }
    if ([_infoType isEqualToString:@"IPADDRESS"] ||
        [_infoType isEqualToString:@"SUBNETMASK"] ||
        [_infoType isEqualToString:@"GATEWAY"] ||
        [_infoType isEqualToString:@"DNS"]) {
        return [self resolveIPFieldString] ?: @"";
    }
    return @"";
}

- (BOOL)resolveIPField {
    return [self resolveIPFieldString] != nil;
}

- (nullable NSString *)resolveIPFieldString {
    // Use getifaddrs to find the Wi-Fi interface address.
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return nil;

    NSString *result = nil;
    for (struct ifaddrs *ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        // Look for "en0" or "en1" (Wi-Fi) interfaces.
        NSString *name = @(ifa->ifa_name);
        if (![name hasPrefix:@"en"]) continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            char buf[INET_ADDRSTRLEN];
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;

            if ([_infoType isEqualToString:@"IPADDRESS"]) {
                if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
                    result = @(buf);
                    break;
                }
            } else if ([_infoType isEqualToString:@"SUBNETMASK"]) {
                struct sockaddr_in *nm = (struct sockaddr_in *)ifa->ifa_netmask;
                if (nm && inet_ntop(AF_INET, &nm->sin_addr, buf, sizeof(buf))) {
                    result = @(buf);
                    break;
                }
            } else if ([_infoType isEqualToString:@"GATEWAY"]) {
                // Gateway is not directly available via getifaddrs.
                // Return a placeholder.
                result = @"0.0.0.0";
                break;
            } else if ([_infoType isEqualToString:@"DNS"]) {
                result = @"0.0.0.0"; // DNS requires SystemConfiguration resolver API
                break;
            }
        }
    }
    freeifaddrs(ifs);
    return result;
}

@end

#pragma mark - String (simple string measure)

@implementation RMMeasureString {
    NSString *_text;
}

- (BOOL)isStringMeasure { return YES; }

- (void)readSubclassOptions {
    _text = [self.parser readString:self.name key:@"Text" default:@""];
}

- (void)updateValue {
    self.value = 0;
    double d = 0;
    if (_text.length > 0 && [[NSScanner scannerWithString:_text] scanDouble:&d]) {
        self.value = d;
    }
}

- (nullable NSString *)rawString {
    return _text ?: @"";
}

@end

#pragma mark - Stub (Plugin / unsupported)

@implementation RMMeasureStub
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @""; }
- (void)updateValue { self.value = 0; }
@end


