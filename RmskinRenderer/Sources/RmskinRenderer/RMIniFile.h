#pragma once

// RMIniFile — Rainmeter-compatible INI lexer.
//
// Rainmeter uses Win32 GetPrivateProfileSection semantics: sections in
// [Brackets]; key=value lines; ';' begins a comment; case-insensitive section
// and key lookup; when a key repeats within a section the FIRST definition
// wins (later ones are ignored, matching Rainmeter's insert-if-absent). Values
// wrapped in a single pair of matching quotes have the quotes stripped.
//
// Section order is preserved (meters are created in file order). This class
// only tokenizes one file; @Include recursion and variable expansion live in
// RMConfigParser.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface RMIniSection : NSObject
@property (nonatomic, copy) NSString *name;              // original-case name
// Ordered keys (original case) and a case-insensitive value map (UPPER key).
@property (nonatomic, strong) NSMutableArray<NSString *> *orderedKeys;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSString *> *values;
- (nullable NSString *)valueForKey:(NSString *)key;      // case-insensitive
- (void)setValue:(NSString *)value forKey:(NSString *)key overwrite:(BOOL)overwrite;
@end

@interface RMIniFile : NSObject

// Ordered sections, plus a case-insensitive index by UPPER section name.
@property (nonatomic, strong, readonly) NSMutableArray<RMIniSection *> *sections;

// Parse raw INI text (already decoded to a string). Appends into this file's
// sections, merging same-named sections. Returns NO only on nil input.
- (BOOL)parseString:(NSString *)text;

// Convenience: load a file from disk (UTF-8, falling back to Latin-1) and parse.
- (BOOL)parseContentsOfFile:(NSString *)path;

- (nullable RMIniSection *)sectionNamed:(NSString *)name;   // case-insensitive
- (RMIniSection *)ensureSectionNamed:(NSString *)name;      // create if absent

@end

NS_ASSUME_NONNULL_END
