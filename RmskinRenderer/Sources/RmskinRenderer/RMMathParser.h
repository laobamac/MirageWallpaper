#pragma once

// RMMathParser — Rainmeter formula evaluator.
//
// Ports Rainmeter's Common/MathParser (a shunting-yard evaluator based on
// ccalc). Supports +, -, *, /, %, unary minus, ** (power), bitwise & | ^ ~,
// << >>, comparisons (= <> < > <= >=), logical && ||, the ternary
// cond ? a : b, parentheses, constants (Pi, e), and common functions
// (round, trunc, floor, ceil, abs, sgn, min, max, clamp, sqrt, sin, cos, tan,
// asin, acos, atan, atan2, rad, deg, log, ln, exp, pow, mod, random).
//
// Variables inside a formula are already expanded to numbers by RMConfigParser
// before evaluation, so this parser only deals with numeric tokens.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Resolves a bare identifier (e.g. a measure name) used as a variable inside a
// formula. Return YES and write *out if the name is known.
typedef BOOL (^RMMathVariableResolver)(NSString *name, double *out);

@interface RMMathParser : NSObject

// Evaluate a formula string. On success returns YES and writes *result.
// On parse error returns NO (and logs at debug level).
+ (BOOL)parse:(NSString *)formula result:(double *)result;

// Evaluate a formula, resolving bare identifiers via the given resolver
// (used by Calc measures to reference other measures by name).
+ (BOOL)parse:(NSString *)formula
variableResolver:(nullable RMMathVariableResolver)resolver
       result:(double *)result;

// True if the string looks like a formula worth evaluating (contains an
// operator / paren / known function), used to avoid mangling plain text.
+ (BOOL)looksLikeFormula:(NSString *)s;

@end

NS_ASSUME_NONNULL_END
