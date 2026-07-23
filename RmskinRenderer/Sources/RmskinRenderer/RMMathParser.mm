#import "RMMathParser.h"
#import "RMLog.h"

#include <cmath>
#include <string>
#include <vector>
#include <cctype>
#include <cstdlib>

// A compact recursive-descent evaluator. Grammar (lowest to highest precedence):
//   ternary := logicOr ( '?' ternary ':' ternary )?
//   logicOr := logicAnd ( '||' logicAnd )*
//   logicAnd:= bitOr ( '&&' bitOr )*
//   bitOr   := bitXor ( '|' bitXor )*
//   bitXor  := bitAnd ( '^' bitAnd )*
//   bitAnd  := equality ( '&' equality )*
//   equality:= relational ( ('='|'<>') relational )*
//   relational := shift ( ('<'|'>'|'<='|'>=') shift )*
//   shift   := additive ( ('<<'|'>>') additive )*
//   additive:= term ( ('+'|'-') term )*
//   term    := power ( ('*'|'/'|'%') power )*
//   power   := unary ( '**' power )?
//   unary   := ('+'|'-'|'~'|'!') unary | primary
//   primary := number | const | func '(' args ')' | '(' ternary ')'

namespace {

struct Eval {
    const char *p;
    const char *end;
    bool ok = true;
    __unsafe_unretained RMMathVariableResolver resolver = nil;

    void skip() { while (p < end && (*p == ' ' || *p == '\t')) ++p; }

    bool match(const char *tok) {
        skip();
        size_t n = strlen(tok);
        if ((size_t)(end - p) >= n && strncmp(p, tok, n) == 0) {
            // Word operators are not used; symbolic only, so no boundary check.
            p += n;
            return true;
        }
        return false;
    }

    char peek() { skip(); return p < end ? *p : '\0'; }

    double parseTernary() {
        double cond = parseLogicOr();
        skip();
        if (p < end && *p == '?') {
            ++p;
            double a = parseTernary();
            skip();
            if (p < end && *p == ':') { ++p; }
            else { ok = false; }
            double b = parseTernary();
            return cond != 0.0 ? a : b;
        }
        return cond;
    }

    double parseLogicOr() {
        double v = parseLogicAnd();
        for (;;) {
            if (match("||")) { double r = parseLogicAnd(); v = (v != 0 || r != 0) ? 1 : 0; }
            else break;
        }
        return v;
    }
    double parseLogicAnd() {
        double v = parseBitOr();
        for (;;) {
            if (match("&&")) { double r = parseBitOr(); v = (v != 0 && r != 0) ? 1 : 0; }
            else break;
        }
        return v;
    }
    double parseBitOr() {
        double v = parseBitXor();
        for (;;) {
            skip();
            if (p < end && *p == '|' && !(p + 1 < end && p[1] == '|')) {
                ++p; double r = parseBitXor();
                v = (double)((long long)v | (long long)r);
            } else break;
        }
        return v;
    }
    double parseBitXor() {
        double v = parseBitAnd();
        for (;;) {
            skip();
            if (p < end && *p == '^') {
                ++p; double r = parseBitAnd();
                v = (double)((long long)v ^ (long long)r);
            } else break;
        }
        return v;
    }
    double parseBitAnd() {
        double v = parseEquality();
        for (;;) {
            skip();
            if (p < end && *p == '&' && !(p + 1 < end && p[1] == '&')) {
                ++p; double r = parseEquality();
                v = (double)((long long)v & (long long)r);
            } else break;
        }
        return v;
    }
    double parseEquality() {
        double v = parseRelational();
        for (;;) {
            if (match("<>") || match("!=")) { double r = parseRelational(); v = (v != r) ? 1 : 0; }
            else if (match("=")) { double r = parseRelational(); v = (v == r) ? 1 : 0; }
            else break;
        }
        return v;
    }
    double parseRelational() {
        double v = parseShift();
        for (;;) {
            if (match("<=")) { double r = parseShift(); v = (v <= r) ? 1 : 0; }
            else if (match(">=")) { double r = parseShift(); v = (v >= r) ? 1 : 0; }
            else {
                skip();
                if (p < end && *p == '<' && !(p + 1 < end && p[1] == '<')) { ++p; double r = parseShift(); v = (v < r) ? 1 : 0; }
                else if (p < end && *p == '>' && !(p + 1 < end && p[1] == '>')) { ++p; double r = parseShift(); v = (v > r) ? 1 : 0; }
                else break;
            }
        }
        return v;
    }
    double parseShift() {
        double v = parseAdditive();
        for (;;) {
            if (match("<<")) { double r = parseAdditive(); v = (double)((long long)v << (long long)r); }
            else if (match(">>")) { double r = parseAdditive(); v = (double)((long long)v >> (long long)r); }
            else break;
        }
        return v;
    }
    double parseAdditive() {
        double v = parseTerm();
        for (;;) {
            skip();
            if (p < end && *p == '+') { ++p; v += parseTerm(); }
            else if (p < end && *p == '-') { ++p; v -= parseTerm(); }
            else break;
        }
        return v;
    }
    double parseTerm() {
        double v = parsePower();
        for (;;) {
            skip();
            if (p < end && *p == '*' && !(p + 1 < end && p[1] == '*')) { ++p; v *= parsePower(); }
            else if (p < end && *p == '/') { ++p; double r = parsePower(); v = (r != 0) ? v / r : 0; }
            else if (p < end && *p == '%') { ++p; double r = parsePower(); v = (r != 0) ? std::fmod(v, r) : 0; }
            else break;
        }
        return v;
    }
    double parsePower() {
        double v = parseUnary();
        if (match("**")) { double r = parsePower(); v = std::pow(v, r); }
        return v;
    }
    double parseUnary() {
        skip();
        if (p < end && *p == '-') { ++p; return -parseUnary(); }
        if (p < end && *p == '+') { ++p; return parseUnary(); }
        if (p < end && *p == '~') { ++p; return (double)(~(long long)parseUnary()); }
        if (p < end && *p == '!') { ++p; return parseUnary() != 0 ? 0 : 1; }
        return parsePrimary();
    }

    std::vector<double> parseArgs() {
        std::vector<double> args;
        skip();
        if (p < end && *p == ')') { ++p; return args; }
        for (;;) {
            args.push_back(parseTernary());
            skip();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ')') { ++p; break; }
            ok = false; break;
        }
        return args;
    }

    double callFunc(const std::string &name, std::vector<double> &a) {
        auto n = name;
        for (auto &c : n) c = (char)tolower(c);
        auto arg = [&](size_t i) -> double { return i < a.size() ? a[i] : 0.0; };
        if (n == "round") {
            if (a.size() >= 2) { double f = std::pow(10, arg(1)); return std::round(arg(0) * f) / f; }
            return std::round(arg(0));
        }
        if (n == "trunc") return std::trunc(arg(0));
        if (n == "floor") return std::floor(arg(0));
        if (n == "ceil")  return std::ceil(arg(0));
        if (n == "abs")   return std::fabs(arg(0));
        if (n == "sgn")   return arg(0) > 0 ? 1 : (arg(0) < 0 ? -1 : 0);
        if (n == "min")   return std::min(arg(0), arg(1));
        if (n == "max")   return std::max(arg(0), arg(1));
        if (n == "clamp") return std::min(std::max(arg(0), arg(1)), arg(2));
        if (n == "sqrt")  return std::sqrt(arg(0));
        if (n == "sin")   return std::sin(arg(0));
        if (n == "cos")   return std::cos(arg(0));
        if (n == "tan")   return std::tan(arg(0));
        if (n == "asin")  return std::asin(arg(0));
        if (n == "acos")  return std::acos(arg(0));
        if (n == "atan")  return std::atan(arg(0));
        if (n == "atan2") return std::atan2(arg(0), arg(1));
        if (n == "rad")   return arg(0) * M_PI / 180.0;
        if (n == "deg")   return arg(0) * 180.0 / M_PI;
        if (n == "log")   return std::log10(arg(0));
        if (n == "ln")    return std::log(arg(0));
        if (n == "exp")   return std::exp(arg(0));
        if (n == "pow")   return std::pow(arg(0), arg(1));
        if (n == "mod")   { double r = arg(1); return r != 0 ? std::fmod(arg(0), r) : 0; }
        if (n == "random") {
            double lo = a.size() >= 1 ? arg(0) : 0;
            double hi = a.size() >= 2 ? arg(1) : 1;
            if (hi < lo) std::swap(lo, hi);
            return lo + ((double)arc4random() / (double)UINT32_MAX) * (hi - lo);
        }
        ok = false;
        return 0;
    }

    double parsePrimary() {
        skip();
        if (p >= end) { ok = false; return 0; }

        // Empty parens "()" → return 0 (same as Rainmeter).
        if (*p == '(') {
            ++p;
            skip();
            if (p < end && *p == ')') { ++p; return 0; }
            double v = parseTernary();
            skip();
            if (p < end && *p == ')') ++p; else ok = false;
            return v;
        }

        // Leading binary operator where a primary/operand is expected (e.g.
        // "(/2+1)" after an undefined #Var# expanded to empty). Rainmeter
        // treats the missing operand as 0 so the rest of the expression parses
        // normally — 0 / 2 + 1 = 1.
        if (*p == '*' || *p == '/' || *p == '%' || *p == '^' ||
            *p == '|' || *p == '&' || *p == '?' || *p == ':' || *p == ',') {
            return 0;
        }
        // Two-char operators whose first char could also appear at start.
        if (p + 1 < end) {
            if ((*p == '<' && p[1] == '<') || (*p == '>' && p[1] == '>')) return 0;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *s = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_')) ++p;
            std::string name(s, p);
            std::string lower = name;
            for (auto &c : lower) c = (char)tolower(c);
            skip();
            if (p < end && *p == '(') {
                ++p;
                std::vector<double> args = parseArgs();
                return callFunc(name, args);
            }
            if (lower == "pi") return M_PI;
            if (lower == "e")  return M_E;
            // Bare identifier — resolve as a variable (e.g. a measure name).
            if (resolver) {
                double out = 0;
                NSString *nm = [NSString stringWithUTF8String:name.c_str()];
                if (nm && resolver(nm, &out)) return out;
            }
            // Unknown identifier — treat as 0 rather than failing hard.
            return 0;
        }

        // Numeric literal (supports hex 0x..).
        char *endp = nullptr;
        double v = strtod(p, &endp);
        if (endp == p) { ok = false; return 0; }
        p = endp;
        return v;
    }
};

} // namespace

@implementation RMMathParser

+ (BOOL)parse:(NSString *)formula result:(double *)result {
    return [self parse:formula variableResolver:nil result:result];
}

+ (BOOL)parse:(NSString *)formula
variableResolver:(nullable RMMathVariableResolver)resolver
       result:(double *)result {
    if (formula.length == 0) return NO;
    std::string s = formula.UTF8String ?: "";
    Eval e;
    e.p = s.c_str();
    e.end = e.p + s.size();
    e.resolver = resolver;
    double v = e.parseTernary();
    e.skip();
    if (!e.ok || e.p != e.end) {
        RMLogDebug(@"formula parse failed: %@", formula);
        return NO;
    }
    if (result) *result = v;
    return YES;
}

+ (BOOL)looksLikeFormula:(NSString *)s {
    if (s.length == 0) return NO;
    static NSCharacterSet *ops = nil;
    if (ops == nil) ops = [NSCharacterSet characterSetWithCharactersInString:@"+-*/%()<>=&|^?:"];
    return [s rangeOfCharacterFromSet:ops].location != NSNotFound;
}

@end
