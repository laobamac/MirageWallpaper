#pragma once

// RMConfigParser — variable / include / section-variable expansion + typed reads.
//
// Owns the merged INI for one skin (main file plus every @Include, resolved
// recursively) and the variable table. Mirrors Rainmeter's ConfigParser:
//
//   * #Var#              user + built-in variables (#@#, #CURRENTPATH#, ...)
//   * [Section]          section variables (measure values / meter geometry),
//     [Section:selector] resolved through `sectionVariableResolver`
//   * @Include           recursive file inclusion (relative to the including file)
//   * ParseFormula       (expr) values are evaluated by RMMathParser
//
// The resolver block is installed by RMSkin so the parser stays independent of
// the measure/meter object graph.

#import <Foundation/Foundation.h>
#import "RMIniFile.h"

NS_ASSUME_NONNULL_BEGIN

@interface RMConfigParser : NSObject

@property (nonatomic, strong, readonly) RMIniFile *ini;   // fully merged
@property (nonatomic, copy)   NSString *resourcesPath;    // #@#   (trailing '/')
@property (nonatomic, copy)   NSString *currentPath;      // dir of main skin file
@property (nonatomic, copy)   NSString *currentFile;      // skin file name
@property (nonatomic, copy)   NSString *currentConfig;    // e.g. "# - TETRAKTYS\\SYSTEM INFO"
@property (nonatomic, copy)   NSString *rootConfigPath;   // skin root folder
@property (nonatomic, copy)   NSString *skinsPath;

// token is the raw inside-brackets text, e.g. "MeasureCPU", "MeasureRAM:2",
// "MeterBG:W", "#SomeVar", "&Measure". Return nil to leave the literal intact.
@property (nonatomic, copy, nullable) NSString *(^sectionVariableResolver)(NSString *token);

// Resolves a bare measure name used as a variable inside a Calc formula.
// Return YES and write *out if the named measure exists. Installed by RMSkin.
@property (nonatomic, copy, nullable) BOOL (^measureValueResolver)(NSString *name, double *out);

// Load the main skin file (resolving @Include recursively) and read [Variables].
- (BOOL)loadSkinFile:(NSString *)path;

// Variable table (case-insensitive).
- (void)setVariable:(NSString *)name value:(NSString *)value;
- (nullable NSString *)variableForName:(NSString *)name;   // built-in or user

// Expand #Var# and [Section] tokens in an arbitrary string.
- (NSString *)expand:(NSString *)raw;

// Typed reads from a section of the merged ini (expanded, formula-aware).
- (nullable NSString *)readString:(NSString *)section key:(NSString *)key default:(nullable NSString *)def;
- (double)readDouble:(NSString *)section key:(NSString *)key default:(double)def;
- (int)readInt:(NSString *)section key:(NSString *)key default:(int)def;
- (BOOL)readBool:(NSString *)section key:(NSString *)key default:(BOOL)def;

// Evaluate a single already-expanded value as number (handles (formula) & plain).
+ (double)evaluateNumber:(NSString *)expanded default:(double)def;

// Resolve a bare measure name to a value string (for %MeasureName% in String meters).
// Returns nil if the measure is not found. Installed by RMSkin.
@property (nonatomic, copy, nullable) NSString *(^measureStringResolver)(NSString *name);

@end

NS_ASSUME_NONNULL_END
