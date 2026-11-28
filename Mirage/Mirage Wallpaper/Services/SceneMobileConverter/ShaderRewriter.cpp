//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "ShaderRewriter.hpp"
#include "JsonCompat.hpp"
#include "MobileInternal.hpp"
#include "ZCompatBuiltins.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mirage::mobile {

namespace {

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool ReplaceFirst(std::string& s, std::string_view from, std::string_view to) {
    size_t pos = s.find(from);
    if (pos == std::string::npos) return false;
    s.replace(pos, from.size(), to);
    return true;
}

std::optional<std::string> CheckZCompatOverride(
    std::string_view norm_path,
    uint64_t workshop_id,
    const std::optional<std::filesystem::path>& external_zcompat_dir) {
    // Split path segments.
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= norm_path.size()) {
        size_t slash = norm_path.find('/', start);
        if (slash == std::string_view::npos) {
            parts.push_back(norm_path.substr(start));
            break;
        }
        parts.push_back(norm_path.substr(start, slash - start));
        start = slash + 1;
    }
    if (parts.size() < 3) return std::nullopt;

    std::string_view file_name = parts.back();
    size_t dot = file_name.rfind('.');
    if (dot == std::string_view::npos) return std::nullopt;
    std::string ext = AsciiLower(file_name.substr(dot + 1));
    if (ext != "frag" && ext != "vert") return std::nullopt;

    std::string asset_id;
    if (parts.size() >= 4 && AsciiEqualsIgnoreCase(parts[parts.size() - 4], "workshop")) {
        asset_id = std::string(parts[parts.size() - 3]);
    } else if (AsciiEqualsIgnoreCase(parts[parts.size() - 3], "workshop")) {
        asset_id = std::string(parts[parts.size() - 2]);
    } else {
        return std::nullopt;
    }

    if (external_zcompat_dir.has_value() && IsSafeEntryName(asset_id)) {
        auto cfg_file = ResolveContainedFile(*external_zcompat_dir, asset_id + "/config.json");
        if (cfg_file) {
            try {
                auto raw = ReadFileBytes(*cfg_file, 1024 * 1024);
                if (auto doc = ParseJsonLenient(raw); doc && doc->is_object()) {
                    uint64_t max_id = std::numeric_limits<uint64_t>::max();
                    if (auto it = doc->find("maximumprojectid"); it != doc->end()) {
                        if (it->is_string()) {
                            try { max_id = std::stoull(it->get_ref<const std::string&>()); } catch (...) {}
                        } else if (it->is_number_unsigned()) {
                            max_id = it->get<uint64_t>();
                        }
                    }
                    if (workshop_id == 0 || workshop_id <= max_id) {
                        if (auto tf = doc->find(ext); tf != doc->end() && tf->is_string()) {
                            auto target = ResolveContainedFile(
                                *external_zcompat_dir,
                                asset_id + "/" + tf->get_ref<const std::string&>());
                            if (target) {
                                auto content = ReadFileBytes(*target, 4 * 1024 * 1024);
                                return std::string(content.begin(), content.end());
                            }
                        }
                    }
                }
            } catch (...) {
            }
        }
    }

    if (asset_id == "2078835426" &&
        (workshop_id == 0 || workshop_id <= 9223372036854775807ULL)) {
        return ext == "frag" ? std::string(kZCompat2078835426Frag) : std::string(kZCompat2078835426Vert);
    }
    if (asset_id == "2084198056" &&
        (workshop_id == 0 || workshop_id <= 2638335396ULL)) {
        return ext == "frag" ? std::string(kZCompat2084198056Frag) : std::string(kZCompat2084198056Vert);
    }
    return std::nullopt;
}

enum class TokKind {
    Directive,
    Comment,
    StringLit,
    Ident,
    IntLit,
    FloatLit,
    HexLit,
    Symbol,
};

struct Token {
    TokKind kind = TokKind::Symbol;
    size_t start = 0;
    size_t length = 0;
};

bool IsIdentStart(char c) noexcept {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) || c == '_';
}

bool IsIdentChar(char c) noexcept {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_';
}

bool IsDigitChar(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool IsHexDigitChar(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::vector<Token> TokenizeShader(std::string_view src) {
    std::vector<Token> tokens;
    tokens.reserve(src.size() / 4);
    size_t i = 0;
    size_t n = src.size();
    bool line_start = true;

    while (i < n) {
        char c = src[i];
        if (c == '\n') {
            line_start = true;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }

        // Preprocessor directive at start of line (including backslash continuations).
        if (line_start && c == '#') {
            size_t start = i;
            ++i;
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n && src[i + 1] == '\n') {
                    i += 2;
                    continue;
                }
                if (src[i] == '\n') break;
                ++i;
            }
            tokens.push_back(Token{TokKind::Directive, start, i - start});
            continue;
        }

        line_start = false;

        // Line comment.
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            size_t start = i;
            i += 2;
            while (i < n && src[i] != '\n') ++i;
            tokens.push_back(Token{TokKind::Comment, start, i - start});
            continue;
        }

        // Block comment.
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t start = i;
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i = std::min(i + 2, n);
            tokens.push_back(Token{TokKind::Comment, start, i - start});
            continue;
        }

        // Quoted string or char literal.
        if (c == '"' || c == '\'') {
            char quote = c;
            size_t start = i++;
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (src[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            tokens.push_back(Token{TokKind::StringLit, start, i - start});
            continue;
        }

        // Identifier.
        if (IsIdentStart(c)) {
            size_t start = i++;
            while (i < n && IsIdentChar(src[i])) ++i;
            tokens.push_back(Token{TokKind::Ident, start, i - start});
            continue;
        }

        // Numeric literal: hex, float, or bare integer.
        if (IsDigitChar(c) || (c == '.' && i + 1 < n && IsDigitChar(src[i + 1]))) {
            size_t start = i;
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                while (i < n && IsHexDigitChar(src[i])) ++i;
                if (i < n && (src[i] == 'u' || src[i] == 'U')) ++i;
                tokens.push_back(Token{TokKind::HexLit, start, i - start});
                continue;
            }
            bool is_float = false;
            if (c == '.') {
                is_float = true;
                ++i;
                while (i < n && IsDigitChar(src[i])) ++i;
            } else {
                while (i < n && IsDigitChar(src[i])) ++i;
                if (i < n && src[i] == '.') {
                    is_float = true;
                    ++i;
                    while (i < n && IsDigitChar(src[i])) ++i;
                }
            }
            if (i < n && (src[i] == 'e' || src[i] == 'E')) {
                size_t saved = i;
                ++i;
                if (i < n && (src[i] == '+' || src[i] == '-')) ++i;
                if (i < n && IsDigitChar(src[i])) {
                    is_float = true;
                    while (i < n && IsDigitChar(src[i])) ++i;
                } else {
                    i = saved;
                }
            }
            bool has_u_suffix = false;
            if (i < n && (src[i] == 'f' || src[i] == 'F')) {
                is_float = true;
                ++i;
            } else if (i < n && (src[i] == 'u' || src[i] == 'U')) {
                has_u_suffix = true;
                ++i;
            }
            TokKind kind = is_float ? TokKind::FloatLit : (has_u_suffix ? TokKind::HexLit : TokKind::IntLit);
            tokens.push_back(Token{kind, start, i - start});
            continue;
        }

        // Multi-character operators we care about (for clean binary-op checking).
        if (i + 1 < n) {
            char c2 = src[i + 1];
            if ((c == '+' || c == '-' || c == '*' || c == '/' || c == '=' || c == '!' ||
                 c == '<' || c == '>') && c2 == '=') {
                tokens.push_back(Token{TokKind::Symbol, i, 2});
                i += 2;
                continue;
            }
            if ((c == '&' && c2 == '&') || (c == '|' && c2 == '|') ||
                (c == '+' && c2 == '+') || (c == '-' && c2 == '-')) {
                tokens.push_back(Token{TokKind::Symbol, i, 2});
                i += 2;
                continue;
            }
        }

        tokens.push_back(Token{TokKind::Symbol, i, 1});
        ++i;
    }

    return tokens;
}

std::string NormalizeTokensForMobileGlslEs(const std::string& src) {
    static const std::unordered_set<std::string_view> kFloatCalls = {
        "vec2", "vec3", "vec4", "mat2", "mat3", "mat4",
        "CAST2", "CAST3", "CAST4",
        "mix", "min", "max", "clamp", "pow", "step", "smoothstep", "mod",
        "abs", "sign", "floor", "trunc", "round", "ceil", "fract", "frac", "saturate",
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
        "exp", "exp2", "log", "log2", "sqrt", "inversesqrt", "lerp", "dot", "cross",
        "length", "distance", "normalize", "reflect", "refract",
    };
    static const std::unordered_set<std::string_view> kIntegerOverloads = {
        "mix", "min", "max", "clamp", "abs", "sign",
    };
    static const std::unordered_set<std::string_view> kFloatingTypes = {
        "float", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4",
        "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4",
        "mat4x2", "mat4x3", "mat4x4",
    };
    static const std::unordered_set<std::string_view> kIntTypes = {
        "int", "uint", "bool", "ivec2", "ivec3", "ivec4",
        "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4",
    };

    auto tokens = TokenizeShader(src);
    auto tok_sv = [&](size_t idx) -> std::string_view {
        return std::string_view(src).substr(tokens[idx].start, tokens[idx].length);
    };

    std::vector<size_t> code;
    code.reserve(tokens.size());
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind != TokKind::Comment && tokens[i].kind != TokKind::Directive) {
            code.push_back(i);
        }
    }

    // Collect variable declarations (including multi-decl & function params) and user-defined float-param signatures.
    std::unordered_map<std::string_view, std::unordered_set<std::string_view>> declared_types;
    std::unordered_map<std::string_view, std::vector<bool>> user_fn_float_params;
    static const std::unordered_set<std::string_view> kBuiltInFloatVars = {
        "gl_FragColor", "gl_Position", "gl_FragCoord", "gl_PointCoord",
        "M_PI", "g_Time", "g_Daytime", "g_Texture0Resolution", "g_Texture1Resolution",
        "g_Texture2Resolution", "g_Texture3Resolution", "g_Screen", "g_PointerPosition",
        "g_PointerPositionLast", "g_PointerState", "g_ParallaxPosition", "g_EffectTextureProjectionMatrix",
        "g_EffectTextureProjectionMatrixInverse", "g_ModelViewProjectionMatrix",
        "g_ModelViewProjectionMatrixInverse", "g_TextureReductionScale",
    };

    auto is_float_swizzle = [](std::string_view sw) -> bool {
        if (sw.empty() || sw.size() > 4) return false;
        for (char c : sw) {
            if (c != 'x' && c != 'y' && c != 'z' && c != 'w' &&
                c != 'r' && c != 'g' && c != 'b' && c != 'a' &&
                c != 's' && c != 't' && c != 'p' && c != 'q') {
                return false;
            }
        }
        return true;
    };

    for (size_t pos = 0; pos + 1 < code.size(); ++pos) {
        std::string_view t = tok_sv(code[pos]);
        if (!kFloatingTypes.count(t) && !kIntTypes.count(t)) continue;
        if (tokens[code[pos + 1]].kind != TokKind::Ident) continue;
        std::string_view ident = tok_sv(code[pos + 1]);

        // Function definition/prototype: `<retType> <fnName> ( <params> )`
        if (pos + 2 < code.size() && tok_sv(code[pos + 2]) == "(") {
            int pdepth = 1;
            size_t k = pos + 3;
            std::vector<bool> param_is_float;
            bool cur_float = false;
            bool has_param_tokens = false;
            while (k < code.size() && pdepth > 0) {
                std::string_view kt = tok_sv(code[k]);
                if (kt == "(") {
                    ++pdepth;
                } else if (kt == ")") {
                    --pdepth;
                    if (pdepth == 0) {
                        if (has_param_tokens) param_is_float.push_back(cur_float);
                        break;
                    }
                } else if (kt == "," && pdepth == 1) {
                    param_is_float.push_back(cur_float);
                    cur_float = false;
                    has_param_tokens = false;
                } else if (pdepth == 1) {
                    has_param_tokens = true;
                    if (kFloatingTypes.count(kt)) cur_float = true;
                    else if (kIntTypes.count(kt)) cur_float = false;
                }
                ++k;
            }
            if (!param_is_float.empty()) {
                auto it = user_fn_float_params.find(ident);
                if (it == user_fn_float_params.end()) {
                    user_fn_float_params[ident] = std::move(param_is_float);
                } else if (it->second.size() == param_is_float.size()) {
                    for (size_t pi = 0; pi < param_is_float.size(); ++pi) {
                        it->second[pi] = it->second[pi] && param_is_float[pi];
                    }
                }
            }
            continue;
        }

        // Variable or parameter declaration (handles `float a, b = 1;` too).
        declared_types[ident].insert(t);
        size_t scan = pos + 2;
        int pdepth = 0;
        int bdepth = 0;
        while (scan < code.size()) {
            std::string_view st = tok_sv(code[scan]);
            if (st == "(") ++pdepth;
            else if (st == ")") {
                if (pdepth == 0) break; // end of parameter list
                --pdepth;
            } else if (st == "[") ++bdepth;
            else if (st == "]") { if (bdepth > 0) --bdepth; }
            else if (st == ";" || st == "{" || st == "}") break;
            else if (st == "," && pdepth == 0 && bdepth == 0) {
                // Only treat as multi-variable declaration if next token is an identifier (not a type keyword).
                if (scan + 1 < code.size() && tokens[code[scan + 1]].kind == TokKind::Ident) {
                    std::string_view next_id = tok_sv(code[scan + 1]);
                    if (!kFloatingTypes.count(next_id) && !kIntTypes.count(next_id) &&
                        next_id != "in" && next_id != "out" && next_id != "inout" &&
                        next_id != "const" && next_id != "highp" && next_id != "mediump" && next_id != "lowp") {
                        declared_types[next_id].insert(t);
                    } else {
                        break;
                    }
                }
            }
            ++scan;
        }
    }

    // Precompute matching parenthesis indices for O(1) expression bounds lookup.
    std::vector<size_t> match_paren(code.size(), std::string::npos);
    {
        std::vector<size_t> stack;
        for (size_t pos = 0; pos < code.size(); ++pos) {
            std::string_view t = tok_sv(code[pos]);
            if (t == "(") {
                stack.push_back(pos);
            } else if (t == ")" && !stack.empty()) {
                size_t open = stack.back();
                stack.pop_back();
                match_paren[open] = pos;
                match_paren[pos] = open;
            }
        }
    }

    // Map from token index to replacement text.
    std::map<size_t, std::string> replacements;

    auto is_float_var = [&](std::string_view name) -> bool {
        if (kBuiltInFloatVars.count(name)) return true;
        auto it = declared_types.find(name);
        if (it == declared_types.end() || it->second.empty()) return false;
        for (auto t : it->second) {
            if (!kFloatingTypes.count(t)) return false;
        }
        return true;
    };

    auto is_int_var = [&](std::string_view name) -> bool {
        auto it = declared_types.find(name);
        if (it == declared_types.end() || it->second.empty()) return false;
        for (auto t : it->second) {
            if (!kIntTypes.count(t)) return false;
        }
        return true;
    };

    auto is_float_lit_or_promoted = [&](size_t ti) -> bool {
        if (tokens[ti].kind == TokKind::FloatLit) return true;
        auto it = replacements.find(ti);
        return it != replacements.end() && it->second.find('.') != std::string::npos;
    };

    // Checks whether the token span `code[lo..hi)` has floating-point type/subexpressions
    // (ignoring nested `int(...)` / `uint(...)` / `bool(...)` constructor arguments).
    auto span_has_float = [&](size_t lo, size_t hi) -> bool {
        size_t p = lo;
        while (p < hi) {
            size_t ti = code[p];
            std::string_view t = tok_sv(ti);
            if (kIntTypes.count(t) && p + 1 < hi && tok_sv(code[p + 1]) == "(" &&
                match_paren[p + 1] != std::string::npos) {
                p = match_paren[p + 1] + 1;
                continue;
            }
            if (is_float_lit_or_promoted(ti)) return true;
            if (is_float_var(t)) return true;
            if (kFloatingTypes.count(t) || t == "CAST2" || t == "CAST3" || t == "CAST4") return true;
            if (t == "texSample2D" || t == "texture2D" || t == "texture") return true;
            if (kFloatCalls.count(t) && !kIntegerOverloads.count(t)) return true;
            if (t == "." && p + 1 < hi && is_float_swizzle(tok_sv(code[p + 1]))) {
                if (p == lo || !is_int_var(tok_sv(code[p - 1]))) return true;
            }
            ++p;
        }
        return false;
    };

    auto has_floating_type = [&](const std::vector<size_t>& arg_positions) -> bool {
        if (arg_positions.empty()) return false;
        return span_has_float(arg_positions.front(), arg_positions.back() + 1);
    };

    // 1. Rename GLSL ES reserved keyword `sample` -> `_sample`.
    for (size_t ti : code) {
        if (tokens[ti].kind == TokKind::Ident && tok_sv(ti) == "sample") {
            replacements[ti] = "_sample";
        }
    }

    // 2. Wallpaper Engine `sampleCount` loop/const conversion (`const int sampleCount` -> `const float sampleCount`).
    for (size_t pos = 0; pos + 2 < code.size(); ++pos) {
        if (tok_sv(code[pos]) == "const" &&
            tok_sv(code[pos + 1]) == "int" &&
            tok_sv(code[pos + 2]) == "sampleCount") {
            replacements[code[pos + 1]] = "float";
            declared_types["sampleCount"].clear();
            declared_types["sampleCount"].insert("float");
            if (pos + 4 < code.size() && tok_sv(code[pos + 3]) == "=" &&
                tokens[code[pos + 4]].kind == TokKind::IntLit) {
                replacements[code[pos + 4]] = std::string(tok_sv(code[pos + 4])) + ".0";
            }
        }
    }
    for (size_t pos = 0; pos + 9 < code.size(); ++pos) {
        // for (int <var> = 0; <var> < sampleCount;
        if (tok_sv(code[pos]) == "for" &&
            tok_sv(code[pos + 1]) == "(" &&
            tok_sv(code[pos + 2]) == "int" &&
            tokens[code[pos + 3]].kind == TokKind::Ident &&
            tok_sv(code[pos + 4]) == "=" &&
            tok_sv(code[pos + 5]) == "0" &&
            tok_sv(code[pos + 6]) == ";" &&
            tokens[code[pos + 7]].kind == TokKind::Ident &&
            tok_sv(code[pos + 3]) == tok_sv(code[pos + 7]) &&
            tok_sv(code[pos + 8]) == "<" &&
            tok_sv(code[pos + 9]) == "sampleCount") {
            replacements[code[pos + 2]] = "float";
            replacements[code[pos + 5]] = "0.0";
        }
    }

    // 3. Track tokens inside `[...]` array subscripts, `for (...)` headers, and `int(...)`/`uint(...)` casts.
    std::vector<bool> inside_brackets(code.size(), false);
    std::vector<bool> inside_for_header(code.size(), false);
    std::vector<bool> inside_int_cast(code.size(), false);
    {
        int bdepth = 0;
        for (size_t pos = 0; pos < code.size(); ++pos) {
            std::string_view t = tok_sv(code[pos]);
            if (t == "[") {
                ++bdepth;
                inside_brackets[pos] = true;
            } else if (t == "]") {
                inside_brackets[pos] = true;
                if (bdepth > 0) --bdepth;
            } else if (bdepth > 0) {
                inside_brackets[pos] = true;
            }
        }
        for (size_t pos = 0; pos + 1 < code.size(); ++pos) {
            if (tok_sv(code[pos]) == "for" && tok_sv(code[pos + 1]) == "(") {
                size_t close = match_paren[pos + 1];
                if (close != std::string::npos) {
                    for (size_t k = pos + 1; k <= close; ++k) inside_for_header[k] = true;
                }
            } else if (kIntTypes.count(tok_sv(code[pos])) && tok_sv(code[pos + 1]) == "(") {
                size_t close = match_paren[pos + 1];
                if (close != std::string::npos) {
                    for (size_t k = pos + 1; k <= close; ++k) inside_int_cast[k] = true;
                }
            }
        }
    }

    auto promote_lit = [&](size_t ti) -> bool {
        if (tokens[ti].kind != TokKind::IntLit) return false;
        if (replacements.find(ti) != replacements.end()) return false;
        replacements[ti] = std::string(tok_sv(ti)) + ".0";
        return true;
    };

    auto is_arith_op = [](std::string_view op) {
        return op == "+" || op == "-" || op == "*" || op == "/" ||
               op == "+=" || op == "-=" || op == "*=" || op == "/=" ||
               op == "<" || op == ">" || op == "<=" || op == ">=";
    };
    auto is_float_stmt_lhs = [&](size_t pos) -> bool {
        size_t s = pos;
        while (s > 0) {
            std::string_view t = tok_sv(code[s - 1]);
            if (t == ";" || t == "{" || t == "}") break;
            --s;
        }
        if (s < code.size()) {
            std::string_view first = tok_sv(code[s]);
            if (first == "const" && s + 1 < code.size()) first = tok_sv(code[s + 1]);
            if (kFloatingTypes.count(first)) return true;
            if (kIntTypes.count(first)) return false;
            if (is_float_var(first)) return true;
        }
        return false;
    };

    // Promote bare integer literals directly inside a float-valued sub-expression `code[lo..hi)`
    // (skipping subscripts, int casts, and integer variables).
    auto promote_arg_if_scalar = [&](const std::vector<size_t>& arg_positions) -> bool {
        size_t lit_pos = std::string::npos;
        if (arg_positions.size() == 1) {
            lit_pos = arg_positions[0];
        } else if (arg_positions.size() == 2 &&
                   (tok_sv(code[arg_positions[0]]) == "+" || tok_sv(code[arg_positions[0]]) == "-")) {
            lit_pos = arg_positions[1];
        }
        if (lit_pos != std::string::npos && !inside_brackets[lit_pos] && !inside_int_cast[lit_pos]) {
            return promote_lit(code[lit_pos]);
        }
        return false;
    };

    // Fixed-point passes (converges in 1-3 iterations) so chained float expressions like
    // `min(1, 1 - sqrt(1 - pow(v, 2)))` and `pointer * 2 - 1` propagate cleanly.
    for (int round = 0; round < 4; ++round) {
        bool changed_this_round = false;

        // 4. Builtin & user-defined float function calls.
        for (size_t pos = 0; pos + 1 < code.size(); ++pos) {
            std::string_view fn = tok_sv(code[pos]);
            if (tok_sv(code[pos + 1]) != "(") continue;
            bool is_builtin_float = kFloatCalls.count(fn) > 0;
            auto ufn_it = user_fn_float_params.find(fn);
            bool is_user_fn = (ufn_it != user_fn_float_params.end());
            if (!is_builtin_float && !is_user_fn) continue;
            // Skip function declarations/definitions (`<type> fn(...)`).
            if (pos > 0 && (kFloatingTypes.count(tok_sv(code[pos - 1])) ||
                            kIntTypes.count(tok_sv(code[pos - 1])) ||
                            tok_sv(code[pos - 1]) == "void")) {
                continue;
            }
            size_t close = match_paren[pos + 1];
            if (close == std::string::npos) continue;

            std::vector<std::vector<size_t>> args;
            {
                int depth = 1;
                int brackets = 0;
                size_t arg_start = pos + 2;
                for (size_t k = pos + 2; k <= close; ++k) {
                    std::string_view t = tok_sv(code[k]);
                    if (t == "(") ++depth;
                    else if (t == "[") ++brackets;
                    else if (t == "]") --brackets;
                    if ((t == "," && depth == 1 && brackets == 0) || (k == close)) {
                        std::vector<size_t> arg;
                        for (size_t a = arg_start; a < k; ++a) arg.push_back(a);
                        args.push_back(std::move(arg));
                        arg_start = k + 1;
                    }
                    if (t == ")") --depth;
                }
            }

            if (is_user_fn) {
                const auto& mask = ufn_it->second;
                if (args.size() == mask.size()) {
                    for (size_t ai = 0; ai < args.size(); ++ai) {
                        if (mask[ai] && promote_arg_if_scalar(args[ai])) {
                            changed_this_round = true;
                        }
                    }
                }
                continue;
            }

            if (kIntegerOverloads.count(fn)) {
                bool any_float = false;
                for (const auto& a : args) {
                    if (has_floating_type(a)) { any_float = true; break; }
                }
                // Also treat integer-overload call as float if directly in a float binary op
                // (e.g. `floatExpr / max(1, NODE_COUNT - 2)`).
                bool float_context = false;
                if (!any_float && pos >= 2 && is_arith_op(tok_sv(code[pos - 1]))) {
                    if (is_float_stmt_lhs(pos)) float_context = true;
                }
                if (!any_float && !float_context) {
                    if (pos >= 3 && tok_sv(code[pos - 1]) == "=" &&
                        kFloatingTypes.count(tok_sv(code[pos - 3]))) {
                        if (replacements.find(code[pos]) == replacements.end()) {
                            replacements[code[pos]] = std::string(tok_sv(code[pos - 3])) + "(" + std::string(fn);
                            replacements[code[close]] = "))";
                            changed_this_round = true;
                        }
                    }
                    continue;
                }
                if (!any_float && float_context) {
                    // Promote bare ints inside `max(1, NODE_COUNT - 2)` when none of the args use int vars.
                    for (const auto& a : args) {
                        bool has_int_v = false;
                        for (size_t p : a) {
                            if (is_int_var(tok_sv(code[p]))) { has_int_v = true; break; }
                        }
                        if (!has_int_v) {
                            for (size_t p : a) {
                                if (!inside_brackets[p] && !inside_int_cast[p] && promote_lit(code[p])) {
                                    changed_this_round = true;
                                }
                            }
                        }
                    }
                    continue;
                }
            }

            for (const auto& a : args) {
                if (promote_arg_if_scalar(a)) {
                    changed_this_round = true;
                }
            }
        }

        // 5. Float variable declarations & assignments (`float x = 1;`, `floatVar = 1;`, `v_TexCoord.z = 0;`).
        for (size_t pos = 0; pos + 2 < code.size(); ++pos) {
            if (inside_for_header[pos] || inside_brackets[pos] || inside_int_cast[pos]) continue;
            if (tok_sv(code[pos]) != "=") continue;

            bool lhs_is_float = false;
            if (pos >= 2 && kFloatingTypes.count(tok_sv(code[pos - 2])) &&
                tokens[code[pos - 1]].kind == TokKind::Ident) {
                lhs_is_float = true;
            } else if (pos >= 1 && is_float_var(tok_sv(code[pos - 1]))) {
                lhs_is_float = true;
            } else if (pos >= 3 && tok_sv(code[pos - 2]) == "." &&
                       is_float_swizzle(tok_sv(code[pos - 1])) &&
                       !is_int_var(tok_sv(code[pos - 3]))) {
                lhs_is_float = true;
            }
            if (!lhs_is_float) continue;

            size_t sign_off = (pos + 1 < code.size() &&
                               (tok_sv(code[pos + 1]) == "+" || tok_sv(code[pos + 1]) == "-")) ? 1 : 0;
            size_t lit_pos = pos + 1 + sign_off;
            if (lit_pos + 1 < code.size() &&
                tokens[code[lit_pos]].kind == TokKind::IntLit &&
                (tok_sv(code[lit_pos + 1]) == ";" || tok_sv(code[lit_pos + 1]) == ",")) {
                if (promote_lit(code[lit_pos])) changed_this_round = true;
            }
        }

        // 6. Floating binary expressions & ternary branches (`1e-5 + 2`, `floatVar * 2`, `2 * abs(x)`,
        //    `saturate(...) - 1`, `pointer * 2.0 - 1`, `cond ? 0.159 : 1`).
        for (size_t pos = 0; pos < code.size(); ++pos) {
            if (inside_brackets[pos] || inside_for_header[pos] || inside_int_cast[pos]) continue;
            size_t ti = code[pos];
            if (tokens[ti].kind != TokKind::IntLit) continue;
            if (replacements.find(ti) != replacements.end()) continue;

            bool promote = false;
            // Check left operator & operand: `<left> <op> <int>`
            if (pos >= 2 && is_arith_op(tok_sv(code[pos - 1]))) {
                size_t left_pos = pos - 2;
                std::string_view left_tok = tok_sv(code[left_pos]);
                if (is_float_lit_or_promoted(code[left_pos])) {
                    promote = true;
                } else if (is_float_var(left_tok)) {
                    promote = true;
                } else if (left_tok == ")" && match_paren[left_pos] != std::string::npos) {
                    size_t open = match_paren[left_pos];
                    bool is_int_ctor = (open > 0 && kIntTypes.count(tok_sv(code[open - 1])) > 0);
                    if (!is_int_ctor) {
                        if (open > 0 && kFloatCalls.count(tok_sv(code[open - 1])) &&
                            !kIntegerOverloads.count(tok_sv(code[open - 1]))) {
                            promote = true;
                        } else if (span_has_float(open + 1, left_pos)) {
                            promote = true;
                        } else if (is_float_stmt_lhs(pos)) {
                            promote = true;
                        }
                    }
                } else if (left_tok == "]") {
                    if (is_float_stmt_lhs(pos)) promote = true;
                } else if (left_pos >= 2 && tok_sv(code[left_pos - 1]) == "." &&
                           is_float_swizzle(left_tok) &&
                           !is_int_var(tok_sv(code[left_pos - 2]))) {
                    promote = true;
                }
            }
            // Check right operator & operand: `<int> <op> <right>`
            if (!promote && pos + 2 < code.size() && is_arith_op(tok_sv(code[pos + 1]))) {
                size_t right_pos = pos + 2;
                std::string_view right_tok = tok_sv(code[right_pos]);
                if (is_float_lit_or_promoted(code[right_pos]) ||
                    is_float_var(right_tok) ||
                    kFloatingTypes.count(right_tok) ||
                    (kFloatCalls.count(right_tok) && !kIntegerOverloads.count(right_tok))) {
                    promote = true;
                } else if (kIntegerOverloads.count(right_tok) &&
                           right_pos + 1 < code.size() && tok_sv(code[right_pos + 1]) == "(" &&
                           match_paren[right_pos + 1] != std::string::npos) {
                    if (span_has_float(right_pos + 2, match_paren[right_pos + 1])) {
                        promote = true;
                    }
                } else if (right_tok == "(" && match_paren[right_pos] != std::string::npos) {
                    if (span_has_float(right_pos + 1, match_paren[right_pos])) {
                        promote = true;
                    }
                } else if (right_pos + 2 < code.size() && tok_sv(code[right_pos + 1]) == "." &&
                           is_float_swizzle(tok_sv(code[right_pos + 2])) &&
                           !is_int_var(right_tok)) {
                    promote = true;
                }
            }
            // Check ternary branches: `? <float> : <int>` or `? <int> : <float>`
            if (!promote && pos >= 2 && tok_sv(code[pos - 1]) == ":") {
                if (is_float_lit_or_promoted(code[pos - 2]) || is_float_var(tok_sv(code[pos - 2]))) {
                    promote = true;
                }
            }
            if (!promote && pos + 2 < code.size() && tok_sv(code[pos + 1]) == ":") {
                if (is_float_lit_or_promoted(code[pos + 2]) || is_float_var(tok_sv(code[pos + 2]))) {
                    promote = true;
                }
            }

            if (promote && promote_lit(ti)) {
                changed_this_round = true;
            }
        }

        if (!changed_this_round) break;
    }

    if (replacements.empty()) return src;

    std::string out;
    out.reserve(src.size() + replacements.size() * 4);
    size_t prev_end = 0;
    for (const auto& [ti, rep] : replacements) {
        const Token& tok = tokens[ti];
        if (tok.start > prev_end) {
            out.append(src, prev_end, tok.start - prev_end);
        }
        out.append(rep);
        prev_end = tok.start + tok.length;
    }
    if (prev_end < src.size()) {
        out.append(src, prev_end, src.size() - prev_end);
    }
    return out;
}

bool ParseMacroArgs(
    std::string_view s,
    size_t open_paren,
    std::vector<std::string_view>& out_args,
    size_t& out_close_paren) {
    out_args.clear();
    size_t depth = 0;
    size_t arg_start = open_paren + 1;
    for (size_t i = open_paren; i < s.size(); ++i) {
        char c = s[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth == 0) return false;
            --depth;
            if (depth == 0) {
                out_args.push_back(s.substr(arg_start, i - arg_start));
                out_close_paren = i;
                return true;
            }
        } else if (c == ',' && depth == 1) {
            out_args.push_back(s.substr(arg_start, i - arg_start));
            arg_start = i + 1;
        }
    }
    return false;
}

std::string ExpandBlendOpacity(std::string_view input) {
    static constexpr std::string_view kNeedle = "BlendOpacity";
    std::string out;
    out.reserve(input.size() + 32);
    size_t pos = 0;
    bool ignore_next = false;

    while (pos < input.size()) {
        if (input[pos] == '#') {
            size_t k = pos + 1;
            if (input.substr(k, 6) == "define") {
                k += 6;
                size_t ws = 0;
                while (k < input.size() && (input[k] == ' ' || input[k] == '\t')) { ++k; ++ws; }
                if (ws > 0 && input.substr(k, kNeedle.size()) == kNeedle) {
                    ignore_next = true;
                }
            }
        }

        if (pos + kNeedle.size() <= input.size() &&
            input.compare(pos, kNeedle.size(), kNeedle) == 0) {
            if (ignore_next) {
                ignore_next = false;
                out.push_back(input[pos++]);
                continue;
            }
            bool prev_ok = (pos == 0) || !IsIdentChar(input[pos - 1]);
            size_t open_paren = pos + kNeedle.size();
            while (open_paren < input.size() &&
                   std::isspace(static_cast<unsigned char>(input[open_paren]))) {
                ++open_paren;
            }
            if (prev_ok && open_paren < input.size() && input[open_paren] == '(') {
                std::vector<std::string_view> args;
                size_t close_paren = 0;
                if (ParseMacroArgs(input, open_paren, args, close_paren) && args.size() == 4) {
                    std::string a = TrimAscii(args[0]);
                    std::string b = TrimAscii(args[1]);
                    std::string f = TrimAscii(args[2]);
                    std::string o = TrimAscii(args[3]);
                    out.append("mix(");
                    out.append(a);
                    out.push_back(',');
                    out.append(f);
                    out.push_back('(');
                    out.append(a);
                    out.push_back(',');
                    out.append(b);
                    out.append("),");
                    out.append(o);
                    out.push_back(')');
                    pos = close_paren + 1;
                    continue;
                }
            }
        }
        out.push_back(input[pos++]);
    }
    return out;
}

} // namespace

bool IsShaderPath(std::string_view rel_path) {
    return EndsWithIgnoreCase(rel_path, ".vert") ||
           EndsWithIgnoreCase(rel_path, ".frag") ||
           EndsWithIgnoreCase(rel_path, ".geom");
}

std::pair<std::string, bool> RewriteShaderForMobile(
    std::string_view rel_path,
    std::string_view source,
    uint64_t workshop_id,
    const std::optional<std::filesystem::path>& external_zcompat_dir) {
    std::string norm = NormalizeRelativeAssetRef(rel_path).value_or(std::string(rel_path));
    size_t last_slash = norm.rfind('/');
    std::string_view file_name = (last_slash == std::string::npos)
        ? std::string_view(norm)
        : std::string_view(norm).substr(last_slash + 1);
    size_t dot = file_name.rfind('.');
    std::string stem = AsciiLower((dot == std::string_view::npos) ? file_name : file_name.substr(0, dot));

    // 0. Check zcompat Workshop shader override; fall through to standard mobile GLSL ES passes.
    std::string s;
    if (auto override_src = CheckZCompatOverride(norm, workshop_id, external_zcompat_dir)) {
        s = std::move(*override_src);
    } else {
        s.assign(source);
    }
    ReplaceAll(s, "\r\n", "\n");

    // 1. Rename user-defined `float mod(` -> `float _deprFn_0(`.
    ReplaceFirst(s, "float mod(", "float _deprFn_0(");

    // 2. Relocate byte-0 `#include "common_blur.h"` below `uniform sampler2D g_Texture0;`.
    static constexpr std::string_view kCommonBlurInclude = "#include \"common_blur.h\"";
    if (s.compare(0, kCommonBlurInclude.size(), kCommonBlurInclude) == 0) {
        ReplaceAll(s, "uniform sampler2D g_Texture0;", "");
        s.insert(0, "uniform sampler2D g_Texture0;\n");
    }

    // 3. Stem-specific Wallpaper Engine mobile compatibility patches.
    if (stem == "foliagesway") {
        if (s.find("offset = saturate((offset - v_Bounds.x) * v_Bounds.y);") != std::string::npos &&
            s.find("varying vec2 v_Bounds;") == std::string::npos) {
            ReplaceFirst(s, "uniform sampler2D g_Texture1;",
                         "varying vec2 v_Bounds;\nuniform sampler2D g_Texture1;");
        }
    } else if (stem == "blend") {
        ReplaceAll(s, "step(0, uv) * step(uv, 1)",
                   "step(CAST2(0.0), uv) * step(uv, CAST2(1.0))");
    } else if (stem == "shift_hue") {
        ReplaceAll(s, "albedo.rgb = mix(albedo, newAlbedo, mask)",
                   "albedo.rgb = mix(albedo.rgb, newAlbedo, mask)");
    } else if (stem == "xray") {
        if (s.find("varying float v_PointerScale;") != std::string::npos) {
            if (s.find("uniform vec4 g_PointerState;") == std::string::npos) {
                ReplaceFirst(s, "varying float v_PointerScale;",
                             "varying float v_PointerScale;\nuniform vec4 g_PointerState;");
            }
            if (s.find("g_PointerState.y") == std::string::npos) {
                if (s.find("unprojectedUVs += 0.5;") != std::string::npos) {
                    ReplaceAll(s, "unprojectedUVs += 0.5;",
                               "unprojectedUVs *= CAST2(1.0 / max(0.00001, g_PointerState.y));\nunprojectedUVs += 0.5;");
                } else {
                    static constexpr std::string_view kPtrScale =
                        "unprojectedUVs *= v_PointerScale * vec2(1.0, v_PointerUV.w);";
                    ReplaceFirst(s, kPtrScale,
                                 "unprojectedUVs *= v_PointerScale * vec2(1.0, v_PointerUV.w);\n\tunprojectedUVs *= CAST2(1.0 / max(0.00001, g_PointerState.y));");
                }
            }
        }
        if (s.find("uniform mat4 g_ModelViewProjectionMatrixInverse;") != std::string::npos &&
            s.find("g_ModelViewProjectionMatrixInverse).xyw") != std::string::npos &&
            s.find("v_PointerUV.xy *= 1.0 / g_Texture0Resolution.xy;") != std::string::npos) {
            if (s.find("uniform mat4 g_EffectTextureProjectionMatrixInverse;") == std::string::npos) {
                ReplaceFirst(s, "uniform mat4 g_ModelViewProjectionMatrixInverse;",
                             "uniform mat4 g_ModelViewProjectionMatrixInverse;\nuniform mat4 g_EffectTextureProjectionMatrixInverse;");
            }
            ReplaceAll(s, "g_ModelViewProjectionMatrixInverse).xyw",
                       "g_EffectTextureProjectionMatrixInverse).xyw");
            ReplaceAll(s, "v_PointerUV.xy *= 1.0 / g_Texture0Resolution.xy;",
                       "v_PointerUV.xy *= 0.5;");
        }
    } else if (stem == "perspective") {
        static const std::regex kPerspectiveMix(
            R"(\bmix\s*\(\s*-1(?:\.0*)?[fF]?\s*,\s*1(?:\.0*)?[fF]?\s*,)");
        if (s.find("uniform vec4 g_Texture0Resolution;") != std::string::npos &&
            std::regex_search(s, kPerspectiveMix)) {
            if (s.find("uniform float g_TextureReductionScale;") == std::string::npos) {
                ReplaceFirst(s, "uniform vec4 g_Texture0Resolution;",
                             "uniform vec4 g_Texture0Resolution;\nuniform float g_TextureReductionScale;");
            }
            s = std::regex_replace(
                s, kPerspectiveMix, "mix(-g_TextureReductionScale, g_TextureReductionScale,");
        }
    } else if (stem == "skew") {
        if (s.find("vec2 textureScale = g_Texture0Resolution.zw;") != std::string::npos &&
            s.find("uniform vec4 g_Texture0Resolution;") != std::string::npos) {
            if (s.find("uniform float g_TextureReductionScale;") == std::string::npos) {
                ReplaceFirst(s, "uniform vec4 g_Texture0Resolution;",
                             "uniform vec4 g_Texture0Resolution;\nuniform float g_TextureReductionScale;");
            }
            ReplaceAll(s, "vec2 textureScale = g_Texture0Resolution.zw;",
                       "vec2 textureScale = g_Texture0Resolution.zw * g_TextureReductionScale;");
        }
    }

    // 4. Safe type-aware GLSL ES token pass.
    s = NormalizeTokensForMobileGlslEs(s);

    // 5. Expand BlendOpacity(a, b, f, o) -> mix(a,f(a,b),o).
    if (s.find("BlendOpacity") != std::string::npos) {
        s = ExpandBlendOpacity(s);
    }

    // 6. Emit CRLF line endings matching wallpaperui.exe.
    std::string crlf;
    crlf.reserve(s.size() + 64);
    for (char c : s) {
        if (c == '\n') {
            crlf.push_back('\r');
        }
        crlf.push_back(c);
    }

    bool changed = (crlf != source);
    return {std::move(crlf), changed};
}

} // namespace mirage::mobile
