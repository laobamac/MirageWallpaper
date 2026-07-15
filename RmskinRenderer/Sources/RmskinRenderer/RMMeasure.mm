#import "RMMeasure.h"
#import "RMConfigParser.h"
#import "RMMathParser.h"
#import "RMLog.h"

#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <ifaddrs.h>
#include <net/if.h>
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
        _updateCounter = 0;
    }
    return self;
}

- (void)readOptions {
    RMConfigParser *cp = self.parser;
    self.disabled = [cp readBool:self.name key:@"Disabled" default:NO];
    self.invert   = [cp readBool:self.name key:@"InvertMeasure" default:NO];
    self.updateDivider = [cp readInt:self.name key:@"UpdateDivider" default:1];
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

#pragma mark - NowPlaying (idle stub: no player integration)

// Returns Rainmeter-like idle values so music widgets render their default
// state (Play button, 0:00 times, "Not Available" via the skin's Substitute)
// even without a connected media player.
@implementation RMMeasureNowPlaying {
    NSString *_playerType;
}
- (BOOL)isStringMeasure { return YES; }
- (void)readSubclassOptions {
    _playerType = ([self.parser readString:self.name key:@"PlayerType" default:@"TITLE"]).uppercaseString;
    self.maxValue = 1;
}
- (void)updateValue { self.value = 0; }
- (nullable NSString *)rawString {
    if ([_playerType isEqualToString:@"STATE"])    return @"0";   // 0 = stopped → Play.png
    if ([_playerType isEqualToString:@"POSITION"]) return @"0:00";
    if ([_playerType isEqualToString:@"DURATION"]) return @"0:00";
    if ([_playerType isEqualToString:@"PROGRESS"]) return @"0";
    // TITLE / ARTIST / ALBUM / COVER / etc. → empty (skin Substitute maps it).
    return @"";
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

#pragma mark - Stub (WebParser / Plugin / unsupported)

@implementation RMMeasureStub
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @""; }
- (void)updateValue { self.value = 0; }
@end

#pragma mark - Stub implementations for Windows-only / unimplemented measure types

@implementation RMMeasureWiFiStatus
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @"N/A"; }
- (void)updateValue { self.value = 0; }
@end

@implementation RMMeasureFolderInfo
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @""; }
- (void)updateValue { self.value = 0; }
@end

@implementation RMMeasureRecycleManager
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @"0 B"; }
- (void)updateValue { self.value = 0; }
@end

@implementation RMMeasureRegistry
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @""; }
- (void)updateValue { self.value = 0; }
@end

@implementation RMMeasureRunCommand
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @""; }
- (void)updateValue { self.value = 0; }
@end

@implementation RMMeasureString
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @""; }
- (void)updateValue { self.value = 0; }
@end

@implementation RMMeasureWebParser
- (BOOL)isStringMeasure { return YES; }
- (nullable NSString *)rawString { return @""; }
- (void)updateValue { self.value = 0; }
@end
