#import "RMIniFile.h"
#import "RMLog.h"

static NSString *RMStripMatchingQuotes(NSString *value);

@implementation RMIniSection

- (instancetype)init {
    if ((self = [super init])) {
        _orderedKeys = [NSMutableArray array];
        _values = [NSMutableDictionary dictionary];
    }
    return self;
}

- (nullable NSString *)valueForKey:(NSString *)key {
    return _values[key.uppercaseString];
}

- (void)setValue:(NSString *)value forKey:(NSString *)key overwrite:(BOOL)overwrite {
    NSString *upper = key.uppercaseString;
    if (_values[upper] != nil) {
        // Key already present: Rainmeter keeps the first definition unless
        // explicitly overwriting (e.g. !SetOption at runtime).
        if (!overwrite) return;
    } else {
        [_orderedKeys addObject:key];
    }
    _values[upper] = value;
}

@end

@implementation RMIniFile {
    NSMutableDictionary<NSString *, RMIniSection *> *_index; // UPPER name -> section
}

- (instancetype)init {
    if ((self = [super init])) {
        _sections = [NSMutableArray array];
        _index = [NSMutableDictionary dictionary];
    }
    return self;
}

- (nullable RMIniSection *)sectionNamed:(NSString *)name {
    return _index[name.uppercaseString];
}

- (RMIniSection *)ensureSectionNamed:(NSString *)name {
    RMIniSection *s = [self sectionNamed:name];
    if (s == nil) {
        s = [RMIniSection new];
        s.name = name;
        [_sections addObject:s];
        _index[name.uppercaseString] = s;
    }
    return s;
}

- (BOOL)parseContentsOfFile:(NSString *)path {
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (data == nil) {
        RMLogWarn(@"cannot read ini: %@", path);
        return NO;
    }
    NSString *text = nil;
    // Detect BOM: Rainmeter settings files (Rainmeter.ini) are often UTF-16 LE.
    const unsigned char *b = (const unsigned char *)data.bytes;
    NSUInteger len = data.length;
    if (len >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        text = [[NSString alloc] initWithData:data encoding:NSUTF16LittleEndianStringEncoding];
    } else if (len >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        text = [[NSString alloc] initWithData:data encoding:NSUTF16BigEndianStringEncoding];
    } else if (len >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    }
    if (text == nil) text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (text == nil) {
        // Auto-detect (handles BOM-less UTF-16 and other encodings).
        NSStringEncoding enc = 0;
        text = [NSString stringWithContentsOfFile:path usedEncoding:&enc error:nil];
    }
    if (text == nil) {
        // Last resort: older skins are Windows-1252 / Latin-1.
        text = [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
    }
    if (text == nil) {
        RMLogWarn(@"cannot decode ini text: %@", path);
        return NO;
    }
    // Strip a leading BOM (U+FEFF). The NSUTF16LittleEndian/BigEndian decoders
    // keep the byte-order mark as the first character, which would otherwise
    // prevent the file's first "[Section]" header from being recognised — and
    // for single-section include files (e.g. a lone "[Variables]") that means
    // the entire file is silently dropped.
    if (text.length > 0 && [text characterAtIndex:0] == 0xFEFF) {
        text = [text substringFromIndex:1];
    }
    return [self parseString:text];
}

- (BOOL)parseString:(NSString *)text {
    if (text == nil) return NO;

    RMIniSection *current = nil;
    __block NSMutableArray<NSString *> *lines = [NSMutableArray array];
    [text enumerateLinesUsingBlock:^(NSString *line, BOOL *stop) {
        [lines addObject:line];
    }];

    for (NSString *rawLine in lines) {
        NSString *line = [rawLine stringByTrimmingCharactersInSet:
                          [NSCharacterSet whitespaceCharacterSet]];
        if (line.length == 0) continue;

        unichar first = [line characterAtIndex:0];
        if (first == ';' || first == '#') {
            // ';' is a comment. A leading '#' at column 0 is NOT a variable here
            // (variables appear inside values); some skins use '#' banner lines,
            // treat a lone leading '#' line without '=' as a comment.
            if (first == ';') continue;
            if (first == '#' && [line rangeOfString:@"="].location == NSNotFound) continue;
        }

        if (first == '[') {
            NSRange close = [line rangeOfString:@"]"];
            if (close.location != NSNotFound && close.location > 1) {
                NSString *name = [line substringWithRange:NSMakeRange(1, close.location - 1)];
                current = [self ensureSectionNamed:name];
            }
            continue;
        }

        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        if (current == nil) continue; // key/value before any section: ignore

        NSString *key = [[line substringToIndex:eq.location]
                         stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSString *value = [line substringFromIndex:eq.location + 1];
        // Rainmeter trims leading whitespace of the value but preserves trailing
        // significant characters; trim both ends for robustness.
        value = [value stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        value = RMStripMatchingQuotes(value);

        if (key.length == 0) continue;
        [current setValue:value forKey:key overwrite:NO];
    }
    return YES;
}

@end

static NSString *RMStripMatchingQuotes(NSString *value) {
    if (value.length >= 2) {
        unichar a = [value characterAtIndex:0];
        unichar b = [value characterAtIndex:value.length - 1];
        if ((a == '"' && b == '"') || (a == '\'' && b == '\'')) {
            return [value substringWithRange:NSMakeRange(1, value.length - 2)];
        }
    }
    return value;
}
