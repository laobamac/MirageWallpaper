module;

#include <rstd/macro.hpp>
#include "Utils/StringUtil.h"

#include <mutex>
#include <unordered_map>

module sr.pkg.parse;
import sr.json;
import sr.spec_texs;
import sr.core;
import sr.types;
import rstd.log;
import rstd.cppstd;
import sr.shader_compile;
import sr.scene;
import sr.pkg_asset_version;
import sr.utils;
import :shader_lex;

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

#define SHADER_DIR    "spvs03"
#define SHADER_SUFFIX "spvs"

using namespace sr;

namespace
{

// Decl scanners over GLSL declaration lines. Each WE shader decl is
// line-scoped; the Cursor primitives in :shader_lex do all char-level work.

struct DeclMatch {
    std::size_t      start;       // offset of leading newline (or 0 at file start)
    std::size_t      end;         // one past trailing `;`
    std::size_t      keep_prefix; // number of bytes from start to preserve when stripping
    std::string_view storage;     // attribute/varying/in/out/uniform
    std::string_view type;
    std::string_view name;
    std::string_view array; // "[N]" or empty
};

// Try to match `[ws]<storage_kw> <type> <name>[opt-array][ws];` on the line
// starting at `line_start`. Anchored — leading non-whitespace fails it.
inline std::optional<DeclMatch>
TryParseDeclLine(std::string_view src, std::size_t line_start,
                 std::initializer_list<std::string_view> storage_kws) {
    std::size_t line_end = src.find('\n', line_start);
    if (line_end == std::string_view::npos) line_end = src.size();
    shader_lex::Cursor c(src.substr(line_start, line_end - line_start));
    c.SkipHSpace();

    std::string_view kw;
    for (auto k : storage_kws) {
        auto s = c.Save();
        if (c.MatchKeyword(k)) {
            kw = k;
            break;
        }
        c.Restore(s);
    }
    if (kw.empty()) return std::nullopt;
    c.SkipHSpace();
    auto tn = shader_lex::ReadTypeName(c);
    if (! tn) return std::nullopt;
    c.SkipHSpace();
    auto array = c.ReadArraySuffix();
    c.SkipHSpace();
    if (! c.MatchChar(';')) return std::nullopt;

    DeclMatch m;
    m.start       = line_start;
    m.end         = line_start + c.Pos();
    m.keep_prefix = 0;
    m.storage     = kw;
    m.type        = tn->type;
    m.name        = tn->name;
    m.array       = array.value_or(std::string_view {});
    return m;
}

// Iterate every line; yield one DeclMatch per matching line. `keep_prefix`
// is 1 when a leading newline exists (so callers stripping decl lines keep
// the newline as a paragraph anchor).
template<typename Fn>
inline void ForEachDeclLine(std::string_view                        src,
                            std::initializer_list<std::string_view> storage_kws, Fn&& fn) {
    shader_lex::LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        if (auto m = TryParseDeclLine(src, w.LineStart(), storage_kws)) {
            DeclMatch out = *m;
            if (w.LineStart() > 0) {
                out.start       = w.LineStart() - 1;
                out.keep_prefix = 1;
            } else {
                out.start       = w.LineStart();
                out.keep_prefix = 0;
            }
            fn(out);
        }
    }
}

inline bool IsSamplerType(std::string_view t) {
    return t == "sampler2D" || t == "sampler3D" || t == "samplerCube" ||
           t == "sampler2DComparison" || t == "sampler2DShadow";
}

// Replace every occurrence of `needle` in `body` with `repl`. The placeholder
// names used by the shader synth pipeline are unique tokens
// (`__SHADER_PLACEHOLD__`), so naive substring substitution is safe.
inline std::string ReplaceAll(std::string body, std::string_view needle, std::string_view repl) {
    if (needle.empty()) return body;
    std::string out;
    out.reserve(body.size());
    std::size_t pos = 0;
    while (true) {
        auto next = body.find(needle, pos);
        if (next == std::string::npos) {
            out.append(body, pos, std::string::npos);
            break;
        }
        out.append(body, pos, next - pos);
        out.append(repl);
        pos = next + needle.size();
    }
    return out;
}

// HLSL prologue. WE shaders are written in a hybrid dialect that already
// uses HLSL idioms (mul, texSample2D, float2/3/4, saturate, lerp, frac,
// [maxvertexcount], OUT.Append); only the residual GLSL bits (vec*, mat*,
// attribute, varying, gl_*) need bridging. Routing VS/FS through glslang's
// HLSL frontend lets the parser handle implicit conversions HLSL allows
// (scalar→vec broadcast on assignment, bool→float, etc.) — which would
// otherwise fault in glslang's strict GLSL mode.
//
// Pipeline: this prologue + the user source is fed through glslang's own
// preprocessor (TShader::preprocess) which expands every #if / #include /
// #define. Then a regex pass extracts the surviving `attribute`/`varying`/
// `uniform` declarations (live code only — combo-gated dead branches are
// gone) and Finalprocessor strips them, then re-emits canonical
// `static TYPE NAME;` decls + a paired Texture2D/SamplerState block + a
// shared cbuffer ww_Uniforms + an HLSL entry-point wrapper (main_vs /
// main_ps) that shuffles between the static globals and the
// SV_*-annotated entry struct.
static constexpr const char* pre_shader_code = R"(// auto-generated WE→HLSL prologue
// glslang's HLSL frontend defaults to row-major matrix packing in SPIR-V
// (RowMajor decoration on cbuffer members). The C++ uploader writes
// column-major data (Eigen default), so force column-major packing here.
#pragma pack_matrix(column_major)
#define HLSL 1
#define GLSL 0
#define highp
#define mediump
#define lowp

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define ivec2 int2
#define ivec3 int3
#define ivec4 int4
#define uvec2 uint2
#define uvec3 uint3
#define uvec4 uint4
#define bvec2 bool2
#define bvec3 bool3
#define bvec4 bool4
#define mat2 float2x2
#define mat3 float3x3
#define mat4 float4x4
// GLSL `matCxR` transforms a C-wide vector into an R-wide vector. HLSL's
// row-vector `mul(v, floatCxR)` has the same shape, so keep the indices in
// source order and let the `_ww_mul` overloads below route WE's calls.
#define mat2x2 float2x2
#define mat3x3 float3x3
#define mat4x4 float4x4
#define mat2x3 float2x3
#define mat2x4 float2x4
#define mat3x2 float3x2
#define mat3x4 float3x4
#define mat4x2 float4x2
#define mat4x3 float4x3

#define CAST2(x) ((float2)(x))
#define CAST3(x) ((float3)(x))
#define CAST4(x) ((float4)(x))
#define CAST3X3(x) ((float3x3)(x))

#define mix(a,b,t) lerp((a),(b),(t))
#define fract frac
#define atan(a,b) atan2((a),(b))
#define dFdx ddx
#define dFdy(x) (-ddy(x))

// GLSL `mod(a, b)` is `a - b * floor(a / b)` and isn't an HLSL builtin
// (HLSL has `fmod`, but it uses trunc for the quotient — different sign
// behavior for negative args). Provide a `mod` function so shaders that
// call it without supplying their own definition still compile. A few WE
// shaders ship their own `float mod(float, float)`; PreShaderHeader scans
// the user source and `#define`s `WW_USER_MOD` before this block when so,
// so our definitions are skipped to avoid redefinition errors.
#ifndef WW_USER_MOD
float  mod(float  a, float  b) { return a - b * floor(a / b); }
float2 mod(float2 a, float2 b) { return a - b * floor(a / b); }
float3 mod(float3 a, float3 b) { return a - b * floor(a / b); }
float4 mod(float4 a, float4 b) { return a - b * floor(a / b); }
float2 mod(float2 a, float  b) { return a - b * floor(a / b); }
float3 mod(float3 a, float  b) { return a - b * floor(a / b); }
float4 mod(float4 a, float  b) { return a - b * floor(a / b); }
#endif
// HLSL has saturate, mul, lerp, frac, ddx/ddy, fwidth, max, min, clip, log10,
// pow as builtins — most of the C++-side overload workarounds needed for the
// GLSL frontend disappear here.

// WE shaders write `mul(vec, matrix)` (HLSL vector-first row-vector
// convention). The C++ side uploads matrices designed for GLSL-style
// column-vector multiply (`MVP * v`). Under HLSL native semantics this
// would compute `transpose(M) * v`, transposing every transform. Overload
// `_ww_mul` for each common type combination — each overload calls HLSL
// native `mul` with operands swapped — then `#define mul _ww_mul` redirects.
float2   _ww_mul(float2   v, float2x2 M) { return mul(M, v); }
float3   _ww_mul(float3   v, float3x3 M) { return mul(M, v); }
float4   _ww_mul(float4   v, float4x4 M) { return mul(M, v); }
float2x2 _ww_mul(float2x2 A, float2x2 B) { return mul(B, A); }
float3x3 _ww_mul(float3x3 A, float3x3 B) { return mul(B, A); }
float4x4 _ww_mul(float4x4 A, float4x4 B) { return mul(B, A); }
float2   _ww_mul(float2x2 M, float2   v) { return mul(v, M); }
float3   _ww_mul(float3x3 M, float3   v) { return mul(v, M); }
float4   _ww_mul(float4x4 M, float4   v) { return mul(v, M); }
// Rectangular variants (`mat4x3 g_Bones[]` -> HLSL float4x3). Vec-first and
// matrix-first WE callsites both appear; keep row-vector semantics so MoltenVK's
// MSL backend does not have to reconstruct a transposed temporary.
float3   _ww_mul(float4   v, float4x3 M) { return mul(v, M); }
float3   _ww_mul(float4x3 M, float4   v) { return mul(v, M); }
float4   _ww_mul(float3   v, float3x4 M) { return mul(v, M); }
float4   _ww_mul(float3x4 M, float3   v) { return mul(v, M); }
// Scalar passthroughs.
float    _ww_mul(float a, float b)   { return a * b; }
float2   _ww_mul(float a, float2 b)  { return a * b; }
float3   _ww_mul(float a, float3 b)  { return a * b; }
float4   _ww_mul(float a, float4 b)  { return a * b; }
float2   _ww_mul(float2 a, float b)  { return a * b; }
float3   _ww_mul(float3 a, float b)  { return a * b; }
float4   _ww_mul(float4 a, float b)  { return a * b; }
// Vector-vector: HLSL `mul(v, v)` returns dot product.
float    _ww_mul(float2 a, float2 b) { return dot(a, b); }
float    _ww_mul(float3 a, float3 b) { return dot(a, b); }
float    _ww_mul(float4 a, float4 b) { return dot(a, b); }
#define mul _ww_mul

// `uniform`, `attribute`, `varying` are intentionally NOT #define'd here.
// glslang's preprocess pass runs over this prologue; if any of them were
// stripped to empty, the post-preprocess regex in Finalprocessor wouldn't
// find live declarations. We let the keywords survive preprocess, strip
// the matching lines, and re-emit canonical `static TYPE NAME;` decls +
// `cbuffer ww_Uniforms` + Texture2D/SamplerState pairs at the placeholder.

// WE-dialect texture sampling. Each `uniform sampler2D NAME` becomes a
// `Texture2D<float4> NAME;` + paired `SamplerState NAME_ww_sampler;` in
// the Finalprocessor synth block; texSample2D thus expands to
// `NAME.Sample(NAME_ww_sampler, uv)`. The `texture()` overloads accept
// vec2/vec3/vec4 UV (HLSL Sample takes float2 — the auto-truncation
// matches what WE shaders rely on for `texture(g_T, v_TexCoord)` when
// v_TexCoord is vec4).
#define texSample2D(t, uv)         ((t).Sample(t##_ww_sampler, (uv)))
#define texSample2DLod(t, uv, lod) ((t).SampleLevel(t##_ww_sampler, (uv), (lod)))
// SampleCmpLevelZero handles the depth-compare semantics that `sampler2DComparison`
// implies in GLSL; the paired sampler is a SamplerComparisonState (see
// HLSLSamplerStateType in WPShaderParser.cpp). uv.xy is the atlas coord,
// uv.z is the depth to compare against.
#define texSample2DCompare(t, uv, ref) ((t).SampleCmpLevelZero(t##_ww_sampler, (uv), (ref)))
#define texture(t, uv)             texSample2D((t), (uv))
#define textureLod(t, uv, lod)     texSample2DLod((t), (uv), (lod))

// PerformLighting_V1 is referenced by WE's generic4/genericparticle PBR
// shaders but its body is normally injected by WE's HLSL toolchain based
// on `LIGHTS_*` combos. We don't have that injection step; stub here so
// compilation succeeds. The stub is "albedo × view-aligned shading" —
// darker than WE but visible.
float3 PerformLighting_V1(float3 worldPos, float3 albedo, float3 normal, float3 viewVector,
                          float3 specularTint, float3 f0, float roughness, float metallic) {
    return albedo * max(dot(normalize(normal), normalize(viewVector)), 0.0);
}
float3 PerformLighting_V1(float3 worldPos, float3 albedo, float3 normal, float3 viewVector,
                          float3 specularTint, float3 f0, float roughness, float metallic,
                          float ao) {
    return albedo * ao * max(dot(normalize(normal), normalize(viewVector)), 0.0);
}

__SHADER_TAIL__
__SHADER_PLACEHOLD__

)";

// VS/FS tail: stage I/O is plumbed by the Finalprocessor synthesizer. It
// strips every `attribute|varying TYPE NAME;` line and re-emits canonical
// `static TYPE NAME;` decls; combo-gated `#if` branches drop their decls
// at preprocess time, so vert/frag stages get a matching live name set.
// The keywords MUST NOT be #define'd here; if they were, the regex would
// see unsubstituted text but the HLSL parser would see the substituted
// text, drifting the two views apart.
static constexpr const char* pre_shader_tail_vert = R"(
static float4 gl_Position;
// Rename the user's main() so a synthesized HLSL entry point can wrap it.
// The wrapper (main_vs) is appended in Finalprocessor.
#define main shader_main
)";

static constexpr const char* pre_shader_tail_frag = R"(
static float4 gl_FragCoord;
static float4 glOutColor;
#define gl_FragColor glOutColor
#define main shader_main
)";

static constexpr const char* pre_shader_tail_geom = R"()";

// HLSL prologue used when type==GEOMETRY. WE's .geom source is a hybrid:
// GLSL-flavoured top-level `in vec4 X;` / `out vec4 X;` decls + HLSL-style
// `[maxvertexcount] void main() { ... IN[0].X ... v.Y = ...; OUT.Append(v); }`
// body. We feed it to glslang's HLSL frontend (EShSourceHlsl); this prologue
// bridges GLSL types/builtins to HLSL and Finalprocessor strips the `in`/`out`
// lines + emits `struct WW_VSOut/WW_PSIn` + `cbuffer ww_Uniforms` + replaces
// `void main()` with the GS entry signature.
static constexpr const char* pre_shader_code_gs_hlsl = R"(// auto-generated WE→HLSL prologue (GS)
// glslang's HLSL frontend defaults to row-major matrix packing in SPIR-V
// (RowMajor decoration on cbuffer members). The VS/FS GLSL synth emits a
// std140 UBO with default column-major matrices and the C++ uploader writes
// column-major data, so a row-major GS reads the transpose. Force column-
// major packing here so the GS sees the same matrix as the rest.
#pragma pack_matrix(column_major)
#define HLSL 1
#define GLSL 0
#define highp
#define mediump
#define lowp
#define vec2 float2
#define vec3 float3
#define vec4 float4
#define ivec2 int2
#define ivec3 int3
#define ivec4 int4
#define mat2 float2x2
#define mat3 float3x3
#define mat4 float4x4
#define mat2x2 float2x2
#define mat3x3 float3x3
#define mat4x4 float4x4
#define mat2x3 float2x3
#define mat2x4 float2x4
#define mat3x2 float3x2
#define mat3x4 float3x4
#define mat4x2 float4x2
#define mat4x3 float4x3
#define CAST2(x)   ((float2)(x))
#define CAST3(x)   ((float3)(x))
#define CAST4(x)   ((float4)(x))
#define CAST3X3(x) ((float3x3)(x))
#define mix(a,b,t) lerp((a),(b),(t))
#define fract      frac
#define atan(a,b)  atan2((a),(b))
#define dFdx       ddx
#define dFdy(x)    (-ddy(x))

// glslang's HLSL frontend always tags cbuffer matrices `RowMajor` in SPIR-V
// regardless of `#pragma pack_matrix` or `column_major` qualifiers (verified
// on glslang 16.3.0). With column-major data uploaded from C++ (Eigen
// default), the shader's effective matrix is the transpose of the source.
// HLSL `mul(M, v)` lowers (via glslang) to `OpVectorTimesMatrix V M`, which
// combined with the implicit transpose yields `source_M * V` — exactly the
// transform WE intends. `_ww_mul` swaps WE's vec-first `mul(v, M)` to that
// form.
float2   _ww_mul(float2   v, float2x2 M) { return mul(M, v); }
float3   _ww_mul(float3   v, float3x3 M) { return mul(M, v); }
float4   _ww_mul(float4   v, float4x4 M) { return mul(M, v); }
float2x2 _ww_mul(float2x2 A, float2x2 B) { return mul(B, A); }
float3x3 _ww_mul(float3x3 A, float3x3 B) { return mul(B, A); }
float4x4 _ww_mul(float4x4 A, float4x4 B) { return mul(B, A); }
float2   _ww_mul(float2x2 M, float2   v) { return mul(v, M); }
float3   _ww_mul(float3x3 M, float3   v) { return mul(v, M); }
float4   _ww_mul(float4x4 M, float4   v) { return mul(v, M); }
float3   _ww_mul(float4   v, float4x3 M) { return mul(v, M); }
float3   _ww_mul(float4x3 M, float4   v) { return mul(v, M); }
float4   _ww_mul(float3   v, float3x4 M) { return mul(v, M); }
float4   _ww_mul(float3x4 M, float3   v) { return mul(v, M); }
float    _ww_mul(float a, float b)        { return a * b; }
float2   _ww_mul(float a, float2 b)       { return a * b; }
float3   _ww_mul(float a, float3 b)       { return a * b; }
float4   _ww_mul(float a, float4 b)       { return a * b; }
float2   _ww_mul(float2 a, float b)       { return a * b; }
float3   _ww_mul(float3 a, float b)       { return a * b; }
float4   _ww_mul(float4 a, float b)       { return a * b; }
float    _ww_mul(float2 a, float2 b)      { return dot(a, b); }
float    _ww_mul(float3 a, float3 b)      { return dot(a, b); }
float    _ww_mul(float4 a, float4 b)      { return dot(a, b); }
#define mul _ww_mul

// `gl_Position` is the SV_Position struct field's GLSL name; rename to the
// canonical struct field name so `IN[0].gl_Position` / `v.gl_Position` both
// resolve correctly.
#define gl_Position _ww_sv_position
#define VS_OUTPUT   WW_VSOut
#define PS_INPUT    WW_PSIn

__SHADER_PLACEHOLD__

)";

inline bool IsShaderTrivia(shader_lex::TokenKind kind) {
    return kind == shader_lex::TokenKind::HSpace || kind == shader_lex::TokenKind::Newline ||
           kind == shader_lex::TokenKind::LineComment ||
           kind == shader_lex::TokenKind::BlockComment;
}

inline shader_lex::Token NextShaderToken(shader_lex::Lexer& lx) {
    return lx.NextSkip(IsShaderTrivia);
}

inline bool TokenIs(shader_lex::Token token, std::string_view text) {
    return token.kind == shader_lex::TokenKind::Ident && token.text == text;
}

inline bool PunctIs(shader_lex::Token token, char c) {
    return token.kind == shader_lex::TokenKind::Punct && token.text.size() == 1 &&
           token.text[0] == c;
}

inline bool Contains(std::span<const std::string> values, std::string_view value) {
    return std::ranges::any_of(values, [&](const std::string& v) {
        return v == value;
    });
}

// Legacy WE shaders sometimes address audio float arrays as std140 vec4 groups.
inline bool IsAudioSpectrumName(std::string_view name) {
    return name == G_AUDIO_SPEC_16_L || name == G_AUDIO_SPEC_16_R || name == G_AUDIO_SPEC_32_L ||
           name == G_AUDIO_SPEC_32_R || name == G_AUDIO_SPEC_64_L || name == G_AUDIO_SPEC_64_R;
}

struct ShaderBracketExpr {
    std::size_t      close_end;
    std::string_view expr;
};

inline std::optional<ShaderBracketExpr> ReadBracketExpr(shader_lex::Lexer& lx,
                                                        std::string_view   src) {
    auto open = NextShaderToken(lx);
    if (! PunctIs(open, '[')) return std::nullopt;

    int         depth      = 1;
    std::size_t expr_start = open.offset + open.text.size();
    for (;;) {
        auto t = lx.Next();
        if (t.kind == shader_lex::TokenKind::Eof) return std::nullopt;
        if (! PunctIs(t, '[') && ! PunctIs(t, ']')) continue;

        if (PunctIs(t, '[')) {
            ++depth;
            continue;
        }

        --depth;
        if (depth == 0) {
            return ShaderBracketExpr {
                .close_end = t.offset + t.text.size(),
                .expr      = src.substr(expr_start, t.offset - expr_start),
            };
        }
    }
}

inline std::vector<shader_lex::Token> ExprTokens(std::string_view expr) {
    std::vector<shader_lex::Token> tokens;
    shader_lex::Lexer              lx(expr);
    for (;;) {
        auto t = NextShaderToken(lx);
        if (t.kind == shader_lex::TokenKind::Eof) break;
        tokens.push_back(t);
    }
    return tokens;
}

inline std::optional<std::string> TryFlattenPackedAudioIndex(std::string_view group,
                                                             std::string_view component) {
    auto g = ExprTokens(group);
    auto c = ExprTokens(component);
    if (g.size() == 3 && c.size() == 3 && g[0].kind == shader_lex::TokenKind::Ident &&
        c[0].kind == shader_lex::TokenKind::Ident && g[0].text == c[0].text && PunctIs(g[1], '/') &&
        PunctIs(c[1], '%') && g[2].kind == shader_lex::TokenKind::Int &&
        c[2].kind == shader_lex::TokenKind::Int && g[2].text == "4" && c[2].text == "4") {
        std::string out = "(int)(";
        out.append(g[0].text);
        out.append(")");
        return out;
    }
    return std::nullopt;
}

inline std::string FlattenAudioSpectrumAccess(std::string_view group, std::string_view component) {
    if (auto exact = TryFlattenPackedAudioIndex(group, component)) return *exact;

    std::string out;
    out.reserve(group.size() + component.size() + 32);
    out.append("((int)(");
    out.append(group);
    out.append(") * 4 + (int)(");
    out.append(component);
    out.append("))");
    return out;
}

inline std::string NormalizePackedAudioSpectrumAccess(std::string_view src) {
    shader_lex::Lexer lx(src);
    std::string       out;
    std::size_t       copied  = 0;
    bool              changed = false;

    for (;;) {
        auto name = lx.Next();
        if (name.kind == shader_lex::TokenKind::Eof) break;
        if (name.kind != shader_lex::TokenKind::Ident || ! IsAudioSpectrumName(name.text)) continue;

        auto save      = lx.Save();
        auto group     = ReadBracketExpr(lx, src);
        auto component = group ? ReadBracketExpr(lx, src) : std::nullopt;
        if (! group || ! component) {
            lx.Restore(save);
            continue;
        }

        out.append(src, copied, name.offset - copied);
        out.append(name.text);
        out.push_back('[');
        out.append(FlattenAudioSpectrumAccess(group->expr, component->expr));
        out.push_back(']');
        copied  = component->close_end;
        changed = true;
    }

    if (! changed) return std::string { src };
    out.append(src, copied, std::string::npos);
    return out;
}

inline bool IsLocalMatrixConstructor(std::string_view name) {
    return name == "mat2" || name == "mat3" || name == "mat4" || name == "mat2x2" ||
           name == "mat2x3" || name == "mat2x4" || name == "mat3x2" || name == "mat3x3" ||
           name == "mat3x4" || name == "mat4x2" || name == "mat4x3" || name == "mat4x4" ||
           name == "float2x2" || name == "float2x3" || name == "float2x4" || name == "float3x2" ||
           name == "float3x3" || name == "float3x4" || name == "float4x2" || name == "float4x3" ||
           name == "float4x4";
}

inline std::string NormalizeLocalMatrixMul(std::string_view src) {
    shader_lex::Lexer lx(src);
    std::string       out;
    std::size_t       copied  = 0;
    bool              changed = false;

    for (;;) {
        auto name = lx.Next();
        if (name.kind == shader_lex::TokenKind::Eof) break;
        if (! TokenIs(name, "mul")) continue;

        auto save = lx.Save();
        auto open = NextShaderToken(lx);
        if (! PunctIs(open, '(')) {
            lx.Restore(save);
            continue;
        }

        int                        depth = 1;
        std::optional<std::size_t> comma_end;
        std::optional<std::size_t> close_start;
        for (;;) {
            auto t = lx.Next();
            if (t.kind == shader_lex::TokenKind::Eof) break;
            if (PunctIs(t, '(') || PunctIs(t, '[') || PunctIs(t, '{')) {
                ++depth;
                continue;
            }
            if (PunctIs(t, ')') || PunctIs(t, ']') || PunctIs(t, '}')) {
                --depth;
                if (depth == 0) {
                    close_start = t.offset;
                    break;
                }
                continue;
            }
            if (depth == 1 && PunctIs(t, ',') && ! comma_end) {
                comma_end = t.offset + t.text.size();
            }
        }

        if (! comma_end || ! close_start) {
            lx.Restore(save);
            continue;
        }

        shader_lex::Lexer probe(src);
        probe.SeekTo(*comma_end);
        auto second = NextShaderToken(probe);
        if (second.kind != shader_lex::TokenKind::Ident ||
            ! IsLocalMatrixConstructor(second.text)) {
            continue;
        }

        out.append(src, copied, second.offset - copied);
        out.append("transpose(");
        out.append(src, second.offset, *close_start - second.offset);
        out.push_back(')');
        copied  = *close_start;
        changed = true;
    }

    if (! changed) return std::string { src };
    out.append(src, copied, std::string::npos);
    return out;
}

inline bool LineDefinesMacro(std::string_view src, std::size_t line_start,
                             std::string_view macro_name) {
    shader_lex::Cursor c(src);
    c.SeekTo(line_start);
    if (! c.MatchHashDirective("define")) return false;
    c.SkipHSpace();
    auto ident = c.ReadIdent();
    return ident && *ident == macro_name;
}

inline std::string UndefBeforeUserMacroDefines(std::string_view src, std::string_view macro_name) {
    bool        changed = false;
    std::string out;
    out.reserve(src.size() + 64);
    shader_lex::LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        if (LineDefinesMacro(src, w.LineStart(), macro_name)) {
            out += "#ifdef ";
            out += macro_name;
            out += "\n#undef ";
            out += macro_name;
            out += "\n#endif\n";
            changed = true;
        }
        out.append(src, w.LineStart(), w.LineEnd() - w.LineStart());
        if (w.LineEnd() < src.size()) out.push_back('\n');
    }
    return changed ? out : std::string { src };
}

inline std::vector<std::string> CollectBuildTangentSpaceVars(std::string_view src) {
    std::vector<std::string> vars;
    shader_lex::Lexer        lx(src);
    for (;;) {
        auto t = NextShaderToken(lx);
        if (t.kind == shader_lex::TokenKind::Eof) break;
        if (! TokenIs(t, "mat3")) continue;

        auto name = NextShaderToken(lx);
        if (name.kind != shader_lex::TokenKind::Ident) continue;
        if (! PunctIs(NextShaderToken(lx), '=')) continue;
        if (! TokenIs(NextShaderToken(lx), "BuildTangentSpace")) continue;
        if (! PunctIs(NextShaderToken(lx), '(')) continue;
        if (! Contains(vars, name.text)) vars.emplace_back(name.text);
    }
    return vars;
}

inline void NormalizeExpandedShaderSource(std::string& src) {
    auto tangent_space_vars = CollectBuildTangentSpaceVars(src);
    if (tangent_space_vars.empty()) return;

    shader_lex::Lexer lx(src);
    std::string       out;
    std::size_t       copied = 0;
    for (;;) {
        auto t = lx.Next();
        if (t.kind == shader_lex::TokenKind::Eof) break;
        if (! TokenIs(t, "mul")) continue;

        auto save  = lx.Save();
        auto open  = NextShaderToken(lx);
        auto arg   = NextShaderToken(lx);
        auto comma = NextShaderToken(lx);
        if (! PunctIs(open, '(') || arg.kind != shader_lex::TokenKind::Ident ||
            ! Contains(tangent_space_vars, arg.text) || ! PunctIs(comma, ',')) {
            lx.Restore(save);
            continue;
        }

        out.append(src, copied, t.offset - copied);
        out.append("mul(transpose(");
        out.append(arg.text);
        out.append("),");
        copied = comma.offset + comma.text.size();
    }
    out.append(src, copied, std::string::npos);
    src = std::move(out);
}

inline std::string PatchCommonPerspectiveInclude(std::string_view include_name, std::string src) {
    if (include_name != "common_perspective.h") return src;
    if (src.find("_ww_perspective_mat") != std::string::npos) return src;

    static constexpr std::string_view helper = R"(
mat3 _ww_perspective_mat(mat3 m) {
#if HLSL
	// Local perspective matrices are not uploaded through a cbuffer, so they
	// must compensate the global WE mul shim explicitly.
	return transpose(m);
#else
	return m;
#endif
}

)";

    std::string out;
    out.reserve(src.size() + helper.size() + 64);
    out.append(helper);

    static constexpr std::string_view needle { "return m;" };
    static constexpr std::string_view repl { "return _ww_perspective_mat(m);" };
    std::size_t                       pos = 0;
    for (;;) {
        std::size_t next = src.find(needle, pos);
        if (next == std::string::npos) {
            out.append(src, pos, std::string::npos);
            break;
        }
        out.append(src, pos, next - pos);
        out.append(repl);
        pos = next + needle.size();
    }
    return out;
}

inline std::string LoadGlslInclude(fs::VFS& vfs, const std::string& input) {
    std::string output;
    output.reserve(input.size());
    std::size_t            pos = 0;
    shader_lex::LineWalker w(input);
    for (; ! w.Done(); w.Step()) {
        shader_lex::Cursor c(input);
        c.SeekTo(w.LineStart());
        if (! c.MatchHashDirective("include")) continue;

        // Emit everything up to the directive line, then resolve the include
        // and append the recursively-expanded body. Bytes after the directive
        // on the same line (rare in practice) are skipped — matching the
        // original behavior.
        output.append(input, pos, w.LineStart() - pos);
        std::string line = input.substr(w.LineStart(), w.LineEnd() - w.LineStart());
        auto        in_p = line.find_first_of('\"');
        auto        in_e = line.find_last_of('\"');
        if (in_p == std::string::npos || in_e == std::string::npos || in_e <= in_p) {
            // Malformed include — preserve verbatim.
            output.append(line);
            pos = w.LineEnd();
            continue;
        }
        std::string includeName = line.substr(in_p + 1, in_e - in_p - 1);
        std::string includeSrc  = fs::GetFileContent(vfs, "/assets/shaders/" + includeName);
        includeSrc              = PatchCommonPerspectiveInclude(includeName, std::move(includeSrc));
        output.append("\n//-----include ");
        output.append(includeName);
        output.append("\n");
        output.append(LoadGlslInclude(vfs, includeSrc));
        // WE shaders routinely pass a vector opacity (opacity * mask) to the
        // scalar ApplyBlending, relying on fxc's implicit vector->scalar
        // truncation. glslang's HLSL frontend won't truncate at the call, so
        // emit forwarding overloads right after the definition. Gate on the
        // directly-loaded file (not the recursively-expanded body) so a parent
        // header that nests common_blending.h doesn't re-inject the overloads.
        if (includeSrc.find("ApplyBlending(const int") != std::string::npos) {
            output.append("\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec2 o) { "
                          "return ApplyBlending(bm, A, B, o.x); }"
                          "\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec3 o) { "
                          "return ApplyBlending(bm, A, B, o.x); }"
                          "\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec4 o) { "
                          "return ApplyBlending(bm, A, B, o.x); }\n");
        }
        output.append("\n//-----include end\n");
        pos = w.LineEnd();
    }
    output.append(input, pos, std::string::npos);
    return output;
}

// ParseWPShader implementation moved to WPShaderParser_Pegtl.cpp.
// Declaration is reachable through sr.pkg.parse via the same module.

// Find a safe spot in `src` to splice an `#include` line into. The chosen
// position lies after every top-level `attribute/varying/uniform/struct`
// declaration, before `void main(`, and outside any `#if/#endif` block.
// Returns 0 when no preceding decls are found or the source has multiple
// entry points (post-include shaders we can't reason about).
inline usize FindIncludeInsertPos(const std::string& src, usize startPos) {
    using shader_lex::PpKind;
    (void)startPos;

    const usize main_pos = src.find("void main(");
    if (main_pos == std::string::npos) return 0;
    if (src.find("void main(", main_pos + 2) != std::string::npos) return 0;

    usize                                     after_pos = std::string::npos;
    std::vector<std::pair<usize, usize>>      if_ranges;
    std::vector<usize>                        if_stack;
    constexpr std::array<std::string_view, 4> kKws { "attribute", "varying", "uniform", "struct" };

    shader_lex::LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        if (w.LineStart() >= main_pos) break;
        usize line_end = std::min(w.LineEnd(), main_pos);

        shader_lex::Cursor c(src);
        c.SeekTo(w.LineStart());
        c.SkipHSpace();
        if (c.Eof() || c.Pos() >= line_end) continue;

        if (c.Peek() == '#') {
            shader_lex::Cursor cc(src);
            cc.SeekTo(w.LineStart());
            auto kind = shader_lex::ClassifyPreproc(cc);
            if (kind == PpKind::If || kind == PpKind::Ifdef || kind == PpKind::Ifndef) {
                if_stack.push_back(w.LineStart());
            } else if (kind == PpKind::Endif) {
                if (! if_stack.empty()) {
                    usize start = if_stack.back();
                    if_stack.pop_back();
                    usize end = (w.LineEnd() < src.size()) ? w.LineEnd() + 1 : w.LineEnd();
                    if_ranges.emplace_back(start, end);
                }
            }
        } else {
            for (auto kw : kKws) {
                shader_lex::Cursor probe(src);
                probe.SeekTo(c.Pos());
                if (probe.MatchKeyword(kw) && probe.Pos() < line_end &&
                    shader_lex::IsHSpace(src[probe.Pos()])) {
                    after_pos = (w.LineEnd() < src.size()) ? w.LineEnd() + 1 : w.LineEnd();
                    break;
                }
            }
        }
    }

    usize pos = (after_pos == std::string::npos) ? 0 : std::min(after_pos, main_pos);
    for (const auto& [s, e] : if_ranges) {
        if (pos > s && pos <= e) pos = e;
    }
    return std::min(pos, main_pos);
}

// Comment out stray `#endif` directives with no matching `#if`. A class of
// WE-shipped community shader templates (audio_bars / dot_matrix / sine_wave
// variants — 244 of the corpus failures pre-fix) has one extra `#endif`
// past the file's last `#if`. WE's HLSL toolchain tolerates this; glslang
// rejects it as a preprocess error. Stack-walk the source, and when
// `#endif` would pop an empty stack, comment the line instead.
inline std::string BalanceConditionals(std::string src) {
    using shader_lex::PpKind;
    int         depth = 0;
    std::string out;
    out.reserve(src.size() + 32);
    shader_lex::LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        shader_lex::Cursor c(src);
        c.SeekTo(w.LineStart());
        auto kind        = shader_lex::ClassifyPreproc(c);
        bool stray_endif = false;
        switch (kind) {
        case PpKind::If:
        case PpKind::Ifdef:
        case PpKind::Ifndef: ++depth; break;
        case PpKind::Endif:
            if (depth == 0)
                stray_endif = true;
            else
                --depth;
            break;
        default: break;
        }
        if (stray_endif) out.append("// (ww stray-endif) ");
        out.append(src, w.LineStart(), w.LineEnd() - w.LineStart());
        if (w.LineEnd() < src.size()) out.push_back('\n');
    }
    return out;
}

inline std::string Preprocessor(const std::string& in_src, ShaderType type, const Combos& combos,
                                WPPreprocessorInfo& process_info) {
    std::string with_prologue = sr::WPShaderParser::PreShaderHeader(in_src, combos, type);

    // `#require` is a WE-specific marker, not a real preprocessor directive.
    // Prefix `//` to neutralize it. Allowed leading horizontal whitespace.
    {
        std::string out;
        out.reserve(with_prologue.size());
        shader_lex::LineWalker w(with_prologue);
        for (; ! w.Done(); w.Step()) {
            shader_lex::Cursor c(with_prologue);
            c.SeekTo(w.LineStart());
            auto saved = c.Save();
            if (c.MatchHashDirective("require")) {
                out.append("//");
            }
            c.Restore(saved);
            out.append(with_prologue, w.LineStart(), w.LineEnd() - w.LineStart());
            if (w.LineEnd() < with_prologue.size()) out.push_back('\n');
        }
        with_prologue = std::move(out);
    }

    with_prologue = BalanceConditionals(std::move(with_prologue));

    // Run glslang's own preprocessor: every `#if SKINNING` / `#if FOG_COMPUTED
    // && (...)` / `#if BLENDMODE == 0` block resolves, combo names (BONECOUNT,
    // …) expand, and `#include`s (already inlined in PreShaderSrc, but
    // harmless to re-run) get handled. The regex extraction below then sees
    // only live declarations.
    // All stages route through glslang's HLSL frontend. Bridging macros in
    // the prologue turn GLSL types/intrinsics into HLSL equivalents.
    vulkan::SourceLang lang = vulkan::SourceLang::Hlsl;
    std::string        src;
    if (! vulkan::Preprocess(with_prologue, type, lang, src)) {
        // Fall through: subsequent compile will fail loudly with the same
        // diagnostics. Keep with_prologue so the failing path matches what
        // a developer would see if they bypassed the preprocess step.
        src = std::move(with_prologue);
    }

    // GS source uses `in`/`out` storage classes; VS/FS use `attribute`/`varying`.
    ForEachDeclLine(src, { "attribute", "varying", "in", "out" }, [&](const DeclMatch& m) {
        // attribute-in-vertex and varying-in-fragment both behave as inputs;
        // varying-in-vertex behaves as output. GS: `in` is input (from VS),
        // `out` is output (to FS).
        bool        is_input = (m.storage == "attribute") ||
                               (m.storage == "varying" && type == ShaderType::FRAGMENT) ||
                               (m.storage == "in" && type == ShaderType::GEOMETRY);
        std::string line(src.substr(m.start, m.end - m.start));
        std::string name(m.name);
        if (is_input)
            process_info.input[name] = std::move(line);
        else
            process_info.output[name] = std::move(line);
    });

    // Non-sampler uniform decls feed Finalprocessor's shared cbuffer.
    // Sampler-typed uniforms are emitted as Texture/SamplerState pairs and
    // captured in active_tex_slots instead.
    ForEachDeclLine(src, { "uniform" }, [&](const DeclMatch& m) {
        if (IsSamplerType(m.type)) {
            // Track active sampler slot if it's a `g_TextureN`.
            constexpr std::string_view kTex { "g_Texture" };
            if (m.name.size() > kTex.size() && m.name.substr(0, kTex.size()) == kTex) {
                std::string_view num  = m.name.substr(kTex.size());
                unsigned         slot = 0;
                auto [ptr, ec]        = std::from_chars(num.data(), num.data() + num.size(), slot);
                if (ec == std::errc()) process_info.active_tex_slots.insert(slot);
            }
            return;
        }
        process_info.uniforms[std::string(m.name)] = std::string(m.type) + std::string(m.array);
    });
    return src;
}

// Pass GLSL type names through unchanged; aliases like `float`/`float2` get
// re-emitted as is for HLSL-flavoured leftovers.
inline std::string ToGLSLType(std::string_view t) {
    if (t == "float2") return "vec2";
    if (t == "float3") return "vec3";
    if (t == "float4") return "vec4";
    if (t == "int2") return "ivec2";
    if (t == "int3") return "ivec3";
    if (t == "int4") return "ivec4";
    if (t == "uint2") return "uvec2";
    if (t == "uint3") return "uvec3";
    if (t == "uint4") return "uvec4";
    if (t == "float2x2") return "mat2";
    if (t == "float3x3") return "mat3";
    if (t == "float4x4") return "mat4";
    return std::string(t);
}

// Inverse of ToGLSLType: bridge GLSL aliases back to HLSL canonical names
// (used by the GS synth which feeds HLSL to glslang's HLSL frontend).
inline std::string ToHLSLType(std::string_view t) {
    if (t == "vec2") return "float2";
    if (t == "vec3") return "float3";
    if (t == "vec4") return "float4";
    if (t == "ivec2") return "int2";
    if (t == "ivec3") return "int3";
    if (t == "ivec4") return "int4";
    if (t == "uvec2") return "uint2";
    if (t == "uvec3") return "uint3";
    if (t == "uvec4") return "uint4";
    if (t == "mat2" || t == "mat2x2") return "float2x2";
    if (t == "mat3" || t == "mat3x3") return "float3x3";
    if (t == "mat4" || t == "mat4x4") return "float4x4";
    if (t == "mat2x3") return "float2x3";
    if (t == "mat2x4") return "float2x4";
    if (t == "mat3x2") return "float3x2";
    if (t == "mat3x4") return "float3x4";
    if (t == "mat4x2") return "float4x2";
    if (t == "mat4x3") return "float4x3";
    return std::string(t);
}

struct IODecl {
    char        storage; // 'a' for attribute, 'v' for varying, 'i' for GS `in`, 'o' for GS `out'
    std::string type;    // GLSL type as captured (vec2/vec4/mat3/...)
    std::string name;
    std::string array; // "[N]" or empty
};

inline char StorageCharFor(const std::string& storage_word) {
    if (storage_word == "attribute") return 'a';
    if (storage_word == "in") return 'i';
    if (storage_word == "out") return 'o';
    return 'v'; // varying
}

struct SamplerDecl {
    std::string sampler_type; // "sampler2D" / "samplerCube" / ...
    std::string name;
};

inline std::pair<std::vector<SamplerDecl>, std::string>
ScanAndStripSamplers(const std::string& src) {
    std::vector<SamplerDecl> decls;
    std::string              out;
    out.reserve(src.size());
    usize cursor = 0;
    ForEachDeclLine(src, { "uniform" }, [&](const DeclMatch& m) {
        if (! IsSamplerType(m.type)) return;
        out.append(src, cursor, m.start - cursor);
        out.append(src, m.start, m.keep_prefix);
        cursor = m.end;
        decls.push_back({ std::string(m.type), std::string(m.name) });
    });
    out.append(src, cursor, std::string::npos);
    return { std::move(decls), std::move(out) };
}

inline const char* HLSLSamplerType(std::string_view glsl) {
    if (glsl == "sampler2D") return "Texture2D<float4>";
    if (glsl == "sampler3D") return "Texture3D<float4>";
    if (glsl == "samplerCube") return "TextureCube<float4>";
    // GLSL shadow / comparison samplers: scalar-result texture with a
    // SamplerComparisonState. We bind a Texture2D<float> and a paired
    // SamplerComparisonState (the latter chosen via HLSLSamplerStateType).
    if (glsl == "sampler2DComparison" || glsl == "sampler2DShadow") return "Texture2D<float>";
    return "Texture2D<float4>";
}

inline const char* HLSLSamplerStateType(std::string_view glsl) {
    if (glsl == "sampler2DComparison" || glsl == "sampler2DShadow") return "SamplerComparisonState";
    return "SamplerState";
}

inline bool IsSamplerCombinedImage(std::string_view glsl) {
    // All sampler types in WE are combined image samplers from the
    // descriptor-set side. The HLSL sampling intrinsic differs (Sample
    // vs SampleCmp) but binding semantics are identical.
    (void)glsl;
    return true;
}

// Strip every `uniform TYPE NAME;` declaration (including samplers — already
// stripped by ScanAndStripSamplers when called in sequence, idempotent). The
// caller re-emits them as members of a shared cbuffer.
inline std::string StripUniforms(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    usize cursor = 0;
    ForEachDeclLine(src, { "uniform" }, [&](const DeclMatch& m) {
        out.append(src, cursor, m.start - cursor);
        out.append(src, m.start, m.keep_prefix);
        cursor = m.end;
    });
    out.append(src, cursor, std::string::npos);
    return out;
}

inline std::optional<IODecl> ParseIODecl(const std::string& line) {
    // Skip leading newline / CR that the capture loops preserved as an anchor.
    std::size_t start = 0;
    while (start < line.size() && shader_lex::IsVSpace(line[start])) ++start;
    auto m = TryParseDeclLine(line, start, { "attribute", "varying", "in", "out" });
    if (! m) return std::nullopt;
    return IODecl { StorageCharFor(std::string(m->storage)),
                    std::string(m->type),
                    std::string(m->name),
                    std::string(m->array) };
}

// (base, components) for a scalar / vector type. Recognizes HLSL (floatN /
// intN / uintN / boolN) and GLSL (vecN / ivecN / uvecN / bvecN) spellings, plus
// the scalar forms. components==0 means "don't widen" (matrices, unknown).
struct ScalarVec {
    std::string_view base;
    unsigned         comps { 0 };
};
inline ScalarVec DecomposeVecType(std::string_view t) {
    if (t.find('x') != std::string_view::npos) return {}; // floatRxC matrix
    auto suffixed = [&](std::string_view base) -> ScalarVec {
        if (t == base) return { base, 1 };
        if (t.size() == base.size() + 1 && t.substr(0, base.size()) == base) {
            char c = t.back();
            if (c >= '2' && c <= '4') return { base, unsigned(c - '0') };
        }
        return {};
    };
    for (std::string_view base : { std::string_view("float"),
                                   std::string_view("int"),
                                   std::string_view("uint"),
                                   std::string_view("bool") }) {
        if (auto r = suffixed(base); r.comps) return r;
    }
    // GLSL vector spellings normalize to the matching HLSL base kind.
    if (t == "vec2" || t == "vec3" || t == "vec4") return { "float", unsigned(t.back() - '0') };
    if (t == "ivec2" || t == "ivec3" || t == "ivec4") return { "int", unsigned(t.back() - '0') };
    if (t == "uvec2" || t == "uvec3" || t == "uvec4") return { "uint", unsigned(t.back() - '0') };
    if (t == "bvec2" || t == "bvec3" || t == "bvec4") return { "bool", unsigned(t.back() - '0') };
    return {};
}

// WE links a varying by name across stages; when the same name is declared with
// different widths (e.g. VS `vec4 v_TexCoord` but FS `vec2 v_TexCoord` that
// still reads `.zw`), the producing stage's wider type is the real interface and
// the narrow consumer just swizzles a subset. fxc tolerates this; glslang
// rejects the out-of-range swizzle. Pick the wider type (same base kind) so both
// stages agree. Falls back to `a` when the types aren't comparable vectors.
inline std::string WiderType(const std::string& a, const std::string& b) {
    ScalarVec da = DecomposeVecType(a), db = DecomposeVecType(b);
    if (da.comps == 0 || db.comps == 0 || da.base != db.base) return a;
    return db.comps > da.comps ? b : a;
}

// Pull all `attribute|varying|in|out TYPE NAME;` decls out, return them
// structured + a copy of the source with the lines removed. Stripping is
// essential: `attribute`/`varying` are not HLSL keywords; the entry-point
// synthesizer re-emits canonical `static TYPE NAME;` decls so it never
// drifts from what DXC's preprocessor actually compiled.
inline std::pair<std::vector<IODecl>, std::string> ScanAndStripIO(const std::string& src) {
    std::vector<IODecl> decls;
    std::string         out;
    out.reserve(src.size());
    usize cursor = 0;
    ForEachDeclLine(src, { "attribute", "varying", "in", "out" }, [&](const DeclMatch& m) {
        out.append(src, cursor, m.start - cursor);
        out.append(src, m.start, m.keep_prefix);
        cursor = m.end;
        decls.push_back({ StorageCharFor(std::string(m.storage)),
                          std::string(m.type),
                          std::string(m.name),
                          std::string(m.array) });
    });
    out.append(src, cursor, std::string::npos);
    return { std::move(decls), std::move(out) };
}

// Synthesizer output split in two: `pre` is `static TYPE NAME;` decls
// that must precede the user `void main()` (HLSL needs identifiers
// declared before use). `post` is the HLSL entry-point wrapper that
// must follow `void main()` so it can call the renamed `shader_main()`.
struct SynthOutput {
    std::string pre;
    std::string post;
};

inline usize ArraySlots(const std::string& arr) {
    if (arr.size() < 3 || arr.front() != '[' || arr.back() != ']') return 1;
    usize n = 0;
    for (usize i = 1; i + 1 < arr.size(); ++i) {
        const char c = arr[i];
        if (c < '0' || c > '9') return 1;
        n = n * 10 + (usize)(c - '0');
    }
    return n > 0 ? n : 1;
}

// Build `layout(location=N) in/out TYPE NAME[arr];` declarations from a
// list of IO decls, with locations assigned alphabetically so neighbouring
// stages agree without explicit coordination. `is_input` picks the storage
// qualifier (in vs out). Returns the joined block.
inline std::string EmitStageIOLayout(std::vector<IODecl> decls, bool is_input) {
    // gl_Position is a GLSL builtin; never re-declare it. _ww_sv_position is
    // the GS-side macro alias for the same slot.
    decls.erase(std::remove_if(decls.begin(),
                               decls.end(),
                               [](const IODecl& d) {
                                   return d.name == "gl_Position" || d.name == "_ww_sv_position";
                               }),
                decls.end());
    std::sort(decls.begin(), decls.end(), [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    });
    const char* qual = is_input ? "in" : "out";
    std::string out;
    usize       loc = 0;
    for (const auto& d : decls) {
        out += "layout(location = " + std::to_string(loc) + ") " + qual + " " + ToGLSLType(d.type) +
               " " + d.name + d.array + ";\n";
        loc += ArraySlots(d.array);
    }
    return out;
}

// HLSL-side struct emission for the GS synth. Drops `[[vk::location(N)]]`
// for the same reason as EmitVSFSStruct — glslang's HLSL frontend collapses
// every element of an explicitly-located array onto the same Location.
inline std::string EmitGSHLSLStruct(std::string_view name, std::vector<IODecl> decls) {
    decls.erase(std::remove_if(decls.begin(),
                               decls.end(),
                               [](const IODecl& d) {
                                   return d.name == "gl_Position" || d.name == "_ww_sv_position";
                               }),
                decls.end());
    std::sort(decls.begin(), decls.end(), [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    });
    std::string out;
    out += "struct ";
    out += name;
    out += " {\n";
    out += "    float4 _ww_sv_position : SV_Position;\n";
    for (const auto& d : decls) {
        out += "    " + ToHLSLType(d.type) + " " + d.name + d.array + " : " + d.name + ";\n";
    }
    out += "};\n";
    return out;
}

// HLSL-side struct emission for VS/FS entry points. Same as the GS variant
// but the SV_Position field is included only when the struct represents a
// VS output / FS input (HLSL needs it for rasterizer setup); attributes
// (VS input) don't carry it.
//
// No `[[vk::location(N)]]` is emitted. glslang's HLSL frontend has a known
// bug: `[[vk::location(N)]] TYPE FIELD[K]` puts every element of the array
// at Location N (instead of N, N+1, …, N+K-1). Dropping the explicit
// attribute lets glslang auto-assign sequential locations in declaration
// order. Both VS and FS sort fields alphabetically over the same
// cross-stage union, so the location assignment is stable across stages.
inline std::string EmitVSFSStruct(std::string_view name, std::vector<IODecl> decls,
                                  bool include_sv_position) {
    decls.erase(std::remove_if(decls.begin(),
                               decls.end(),
                               [](const IODecl& d) {
                                   return d.name == "gl_Position" || d.name == "_ww_sv_position";
                               }),
                decls.end());
    std::sort(decls.begin(), decls.end(), [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    });
    std::string out;
    out += "struct ";
    out += name;
    out += " {\n";
    if (include_sv_position) {
        out += "    float4 _ww_sv_position : SV_Position;\n";
    }
    for (const auto& d : decls) {
        out += "    " + ToHLSLType(d.type) + " " + d.name + d.array + " : " + d.name + ";\n";
    }
    out += "};\n";
    return out;
}

// Emit the HLSL synth block (pre = decls / structs / cbuffer / samplers,
// post = entry-point wrapper) for VS or FS. Locations are alphabetical so
// vert/frag stages agree without explicit coordination — both are called
// with the same cross-stage varying union.
inline SynthOutput SynthesizeHLSLEntry(ShaderType stage, std::vector<IODecl> attrs,
                                       std::vector<IODecl> varyings) {
    SynthOutput so;
    if (stage == ShaderType::GEOMETRY) return so;

    // gl_Position propagates via the SV_Position field, not a regular slot.
    // Filter both names (the GS prologue rewrites `gl_Position` to
    // `_ww_sv_position`, so its post-preprocess form needs filtering too).
    auto drop_position = [](std::vector<IODecl>& v) {
        v.erase(std::remove_if(v.begin(),
                               v.end(),
                               [](const IODecl& d) {
                                   return d.name == "gl_Position" || d.name == "_ww_sv_position";
                               }),
                v.end());
    };
    drop_position(attrs);
    drop_position(varyings);

    auto by_name = [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    };
    std::sort(attrs.begin(), attrs.end(), by_name);
    std::sort(varyings.begin(), varyings.end(), by_name);

    // Static globals so the user shader body resolves `a_Position`,
    // `v_TexCoord`, etc. regardless of #if-branch visibility — the wrapper
    // copies from/to the entry struct.
    so.pre += "\n// === auto-generated stage I/O statics ===\n";
    for (const auto& d : attrs) {
        so.pre += "static " + ToHLSLType(d.type) + " " + d.name + d.array + ";\n";
    }
    for (const auto& d : varyings) {
        so.pre += "static " + ToHLSLType(d.type) + " " + d.name + d.array + ";\n";
    }

    std::string& out = so.post;
    out += "\n// === auto-generated entry point ===\n";
    if (stage == ShaderType::VERTEX) {
        out += EmitVSFSStruct("WW_VSIn", attrs, /*sv_pos=*/false);
        out += EmitVSFSStruct("WW_VSOut", varyings, /*sv_pos=*/true);
        out += "WW_VSOut main_vs(WW_VSIn _ww_in) {\n";
        for (const auto& a : attrs) {
            out += "    " + a.name + " = _ww_in." + a.name + ";\n";
        }
        out += "    shader_main();\n";
        out += "    WW_VSOut _ww_out;\n";
        out += "    _ww_out._ww_sv_position = gl_Position;\n";
        for (const auto& v : varyings) {
            if (v.name == "gl_Position" || v.name == "_ww_sv_position") continue;
            out += "    _ww_out." + v.name + " = " + v.name + ";\n";
        }
        out += "    return _ww_out;\n";
        out += "}\n";
    } else { // FRAGMENT
        out += EmitVSFSStruct("WW_PSIn", varyings, /*sv_pos=*/true);
        out += "float4 main_ps(WW_PSIn _ww_in) : SV_Target0 {\n";
        out += "    gl_FragCoord = _ww_in._ww_sv_position;\n";
        for (const auto& v : varyings) {
            if (v.name == "gl_Position" || v.name == "_ww_sv_position") continue;
            out += "    " + v.name + " = _ww_in." + v.name + ";\n";
        }
        out += "    shader_main();\n";
        out += "    return glOutColor;\n";
        out += "}\n";
    }
    return so;
}

// Find a literal `void main()` call in `src` (no regex). Replace with the GS
// entry-point signature. Returns the modified source unchanged if no match.
inline std::string RewriteGSMain(std::string src) {
    static constexpr std::string_view marker { "void main()" };
    static constexpr std::string_view repl {
        "void main_gs(point WW_VSOut IN[1], inout TriangleStream<WW_PSIn> OUT)"
    };
    if (auto pos = src.find(marker); pos != std::string::npos) {
        src.replace(pos, marker.size(), repl);
    }
    return src;
}

// std140 base alignment + size for one element (not the array — caller
// scales). HLSL form (`floatRxC`, `floatN`, scalars). Unknown types fall back
// to vec4-equivalent which is always safely-aligned, never under-padded.
struct Std140Layout {
    std::size_t align;
    std::size_t size;
};
inline Std140Layout Std140Base(std::string_view hlsl_base) {
    if (hlsl_base == "float" || hlsl_base == "int" || hlsl_base == "uint" || hlsl_base == "bool")
        return { 4, 4 };
    if (hlsl_base == "float2" || hlsl_base == "int2" || hlsl_base == "uint2") return { 8, 8 };
    if (hlsl_base == "float3" || hlsl_base == "int3" || hlsl_base == "uint3") return { 16, 12 };
    if (hlsl_base == "float4" || hlsl_base == "int4" || hlsl_base == "uint4") return { 16, 16 };
    // WE/GLSL matrix names are kept in source order for HLSL row-vector math
    // (`matCxR` -> `floatCxR`). std140 still stores C columns, each padded to
    // 16 bytes, so the first HLSL index is the storage column count here.
    if (hlsl_base.size() == 8 && hlsl_base.substr(0, 5) == "float" && hlsl_base[6] == 'x' &&
        hlsl_base[5] >= '2' && hlsl_base[5] <= '4' && hlsl_base[7] >= '2' && hlsl_base[7] <= '4') {
        std::size_t cols = (std::size_t)(hlsl_base[5] - '0');
        return { 16, cols * 16 };
    }
    return { 16, 16 };
}

inline std::pair<std::string_view, std::string_view> SplitUniformType(std::string_view ty) {
    if (auto pos = ty.find('['); pos != std::string_view::npos) {
        return { ty.substr(0, pos), ty.substr(pos) };
    }
    return { ty, {} };
}

struct UniformLayout {
    std::string_view array;
    std::string      hlsl_ty;
    std::size_t      align;
    std::size_t      size;
};

inline std::size_t ParseArrayCount(std::string_view arr) {
    if (arr.size() < 3 || arr.front() != '[' || arr.back() != ']') return 1;
    std::string_view inner = arr.substr(1, arr.size() - 2);
    std::size_t      n     = 0;
    for (char c : inner) {
        if (c == ' ' || c == '\t') continue;
        if (c < '0' || c > '9') return 1;
        n = n * 10 + (std::size_t)(c - '0');
    }
    return n == 0 ? 1 : n;
}

inline UniformLayout LayoutUniform(std::string_view ty) {
    const auto [base_ty, array] = SplitUniformType(ty);
    auto       hlsl_ty          = ToHLSLType(base_ty);
    const auto n                = ParseArrayCount(array);
    const auto L                = Std140Base(hlsl_ty);
    return {
        .array   = array,
        .hlsl_ty = std::move(hlsl_ty),
        .align   = (n > 1) ? std::size_t(16) : L.align,
        .size    = (n > 1) ? ((L.size + 15) & ~std::size_t(15)) * n : L.size,
    };
}

inline void MergeUniform(Map<std::string, std::string>& uniforms_union, std::string_view name,
                         std::string_view ty) {
    auto [it, inserted] = uniforms_union.try_emplace(std::string(name), std::string(ty));
    if (inserted) return;

    const auto old_layout = LayoutUniform(it->second);
    const auto new_layout = LayoutUniform(ty);
    if (new_layout.size > old_layout.size ||
        (new_layout.size == old_layout.size && new_layout.align > old_layout.align)) {
        it->second = std::string(ty);
    }
}

// Emit `cbuffer ww_Uniforms { ... };` body with explicit std140 `:packoffset`
// per member. glslang's HLSL frontend hard-codes HLSL cbuffer packing on
// HLSL sources (see ShaderLang.cpp `setHlslOffsets` when EShSourceHlsl);
// without `packoffset`, scalars get packed into the trailing padding of
// vec3 / vec3[] members, and the C++ uploader (which writes contiguous
// `stride*N` blocks) silently corrupts those neighbours. Annotating each
// member with the std140 (register, component) overrides the auto-packing.
inline std::string EmitCBufferStd140(const Map<std::string, std::string>& uniforms_union) {
    std::string out;
    out += "[[vk::binding(0, 0)]] cbuffer ww_Uniforms {\n";
    std::size_t offset = 0;
    for (const auto& [name, ty] : uniforms_union) {
        const auto  layout    = LayoutUniform(ty);
        std::string array     = std::string(layout.array);
        offset                = (offset + layout.align - 1) & ~(layout.align - 1);
        std::size_t reg       = offset / 16;
        std::size_t comp      = (offset % 16) / 4;
        const char  letter    = "xyzw"[comp];
        const bool  is_square_matrix = layout.hlsl_ty == "float2x2" ||
                                      layout.hlsl_ty == "float3x3" ||
                                      layout.hlsl_ty == "float4x4";
        const bool is_rect_matrix = layout.hlsl_ty == "float2x3" ||
                                    layout.hlsl_ty == "float2x4" ||
                                    layout.hlsl_ty == "float3x2" ||
                                    layout.hlsl_ty == "float3x4" ||
                                    layout.hlsl_ty == "float4x2" ||
                                    layout.hlsl_ty == "float4x3";
        out += "    ";
        if (is_square_matrix) out += "column_major ";
        // MoltenVK/SPIRV-Cross flips column_major rectangular cbuffer matrices
        // relative to identical local HLSL matrix types. WE's rectangular
        // uniforms are std140 column arrays (notably mat4x3 g_Bones), which
        // match HLSL row_major floatCxR storage and keep the Metal type shape
        // consistent with local temporaries.
        if (is_rect_matrix) out += "row_major ";
        out += layout.hlsl_ty + " " + name + array;
        out += " : packoffset(c" + std::to_string(reg);
        if (comp != 0) {
            out += ".";
            out += letter;
        }
        out += ");\n";
        offset += layout.size;
    }
    out += "};\n";
    return out;
}

inline std::string
Finalprocessor(const WPShaderUnit& unit, const WPPreprocessorInfo* pre,
               const WPPreprocessorInfo*            next,
               const Map<std::string, std::string>* uniforms_union_in = nullptr,
               const std::vector<IODecl>*           varying_union_in  = nullptr) {
    // GS: feed glslang's HLSL frontend. Strip GLSL-style top-level `in`/`out`
    // decls, emit HLSL structs (WW_VSOut/WW_PSIn) + ww_Uniforms cbuffer, and
    // rewrite `void main()` to the entry signature `point WW_VSOut IN[1],
    // inout TriangleStream<WW_PSIn> OUT`.
    if (unit.stage == ShaderType::GEOMETRY) {
        auto [io_decls, stripped] = ScanAndStripIO(unit.src);
        std::string body          = StripUniforms(stripped);

        std::vector<IODecl> in_decls, out_decls;
        auto                add_to = [](std::vector<IODecl>& v, const IODecl& d) {
            for (auto& e : v) {
                if (e.name == d.name) {
                    e.type = WiderType(e.type, d.type);
                    return;
                }
            }
            v.push_back(d);
        };
        auto add_in = [&](const IODecl& d) {
            add_to(in_decls, d);
        };
        auto add_out = [&](const IODecl& d) {
            add_to(out_decls, d);
        };
        for (const auto& d : io_decls) {
            if (d.storage == 'i')
                add_in(d);
            else if (d.storage == 'o')
                add_out(d);
        }
        if (pre)
            for (auto& [k, v] : pre->output) {
                if (auto d = ParseIODecl(v); d) add_in(*d);
            }
        if (next)
            for (auto& [k, v] : next->input) {
                if (auto d = ParseIODecl(v); d) add_out(*d);
            }

        std::string synth;
        synth += "\n// === auto-generated GS stage I/O (HLSL) ===\n";
        synth += EmitGSHLSLStruct("WW_VSOut", std::move(in_decls));
        synth += EmitGSHLSLStruct("WW_PSIn", std::move(out_decls));

        // Cross-stage uniform union as an HLSL cbuffer matching VS/FS UBO
        // layout (binding=0, set=0). std140 / column-major matches the
        // glslang GLSL-side block; uploader writes one buffer used by all
        // stages.
        Map<std::string, std::string> uniforms_union_local;
        if (! uniforms_union_in) {
            auto absorb = [&](const Map<std::string, std::string>& m) {
                for (const auto& [k, v] : m) MergeUniform(uniforms_union_local, k, v);
            };
            absorb(unit.preprocess_info.uniforms);
            if (pre) absorb(pre->uniforms);
            if (next) absorb(next->uniforms);
        }
        const Map<std::string, std::string>& uniforms_union =
            uniforms_union_in ? *uniforms_union_in : uniforms_union_local;
        if (! uniforms_union.empty()) {
            synth += "\n// === auto-generated shared uniforms (HLSL, std140 via packoffset) ===\n";
            synth += EmitCBufferStd140(uniforms_union);
        }

        body = RewriteGSMain(std::move(body));
        return ReplaceAll(std::move(body), SHADER_PLACEHOLD, synth);
    }

    // Strip `attribute/varying` lines and collect them as structured decls.
    auto [io_decls, stage1] = ScanAndStripIO(unit.src);

    // Strip `uniform sampler2D NAME;` lines; re-emitted below as explicit
    // `layout(set=0, binding=N) uniform sampler2D NAME;` decls.
    auto [sampler_decls, stage2] = ScanAndStripSamplers(stage1);

    // Strip non-sampler `uniform TYPE NAME;` lines too; re-emitted as
    // members of a cross-stage UBO `ww_Uniforms` at (set=0, binding=0).
    std::string stage3 = StripUniforms(stage2);

    // Partition IO decls into VS-attributes (`a` storage) and varyings
    // (everything else). The cross-stage union ensures vert and frag pick
    // identical location indices alphabetically.
    std::vector<IODecl> attrs;
    std::vector<IODecl> varyings = varying_union_in ? *varying_union_in : std::vector<IODecl> {};
    auto                add_to   = [](std::vector<IODecl>& v, const IODecl& d) {
        for (auto& e : v) {
            if (e.name == d.name) {
                e.type = WiderType(e.type, d.type);
                return;
            }
        }
        v.push_back(d);
    };
    auto add = [&](const IODecl& d) {
        if (d.storage == 'a')
            add_to(attrs, d);
        else
            add_to(varyings, d);
    };
    for (const auto& d : io_decls) add(d);
    auto add_from_line = [&](const std::string& line) {
        if (auto d = ParseIODecl(line); d) add(*d);
    };
    if (! varying_union_in && pre)
        for (auto& [k, v] : pre->output) add_from_line(v);
    if (! varying_union_in && next)
        for (auto& [k, v] : next->input) add_from_line(v);

    // Synthesize the HLSL entry point: static globals for every attr /
    // varying, WW_VSIn/WW_VSOut/WW_PSIn structs, and a main_vs / main_ps
    // wrapper that copies between the struct and the statics.
    SynthOutput synth = SynthesizeHLSLEntry(unit.stage, attrs, varyings);

    // Cross-stage uniform union → single HLSL cbuffer at (set=0, binding=0).
    // Uses the global union from CompileToSpv when provided so VS / GS / FS
    // all see the same offsets (alphabetical, identical layout).
    Map<std::string, std::string> uniforms_union_local;
    if (! uniforms_union_in) {
        auto absorb = [&](const Map<std::string, std::string>& m) {
            for (const auto& [k, v] : m) MergeUniform(uniforms_union_local, k, v);
        };
        absorb(unit.preprocess_info.uniforms);
        if (pre) absorb(pre->uniforms);
        if (next) absorb(next->uniforms);
    }
    const Map<std::string, std::string>& uniforms_union =
        uniforms_union_in ? *uniforms_union_in : uniforms_union_local;

    std::string uniform_block;
    if (! uniforms_union.empty()) {
        uniform_block +=
            "\n// === auto-generated shared uniforms (HLSL, std140 via packoffset) ===\n";
        uniform_block += EmitCBufferStd140(uniforms_union);
    }

    // Texture2D + paired SamplerState per stripped sampler. Bindings start
    // at 1 (binding 0 holds the ww_Uniforms cbuffer). `vk::combinedImageSampler`
    // marks the pair as a single descriptor on the Vulkan side.
    Set<std::string> sampler_seen;
    std::string      sampler_block;
    if (! sampler_decls.empty()) sampler_block += "\n// === auto-generated samplers (HLSL) ===\n";
    usize sampler_idx = 1;
    for (const auto& s : sampler_decls) {
        if (! sampler_seen.insert(s.name).second) continue;
        const char* tex_ty   = HLSLSamplerType(s.sampler_type);
        const char* state_ty = HLSLSamplerStateType(s.sampler_type);
        sampler_block += "[[vk::combinedImageSampler]][[vk::binding(" +
                         std::to_string(sampler_idx) + ", 0)]] " + tex_ty + " " + s.name + ";\n";
        sampler_block += "[[vk::combinedImageSampler]][[vk::binding(" +
                         std::to_string(sampler_idx) + ", 0)]] " + state_ty + " " + s.name +
                         "_ww_sampler;\n";
        ++sampler_idx;
    }

    // Splice synth.pre into the placeholder slot, then append synth.post
    // (which contains the entry-point wrapper that has to follow the user's
    // shader_main()).
    std::string with_decls =
        ReplaceAll(stage3, SHADER_PLACEHOLD, synth.pre + uniform_block + sampler_block);
    return with_decls + synth.post;
}

inline std::string GenSha1(std::span<const WPShaderUnit> units) {
    static constexpr std::string_view kShaderCacheSalt {
        "SceneRenderer-HLSL-MoltenVK-rect-matrix-v3"
    };
    std::string shas(kShaderCacheSalt);
    for (auto& unit : units) {
        shas += utils::genSha1(unit.src);
    }
    return utils::genSha1(shas);
}
inline std::string GetCachePath(std::string_view scene_id, std::string_view filename) {
    return std::string("/cache/") + std::string(scene_id) + "/" SHADER_DIR "/" +
           std::string(filename) + "." SHADER_SUFFIX;
}

inline bool LoadShaderFromFile(std::vector<ShaderCode>& codes, fs::IBinaryStream& file) {
    codes.clear();
    i32 ver = ReadShaderCacheVersion(file);

    usize count = file.ReadUint32();
    rstd_assert(count <= 16 && count >= 0);
    if (count > 16) return false;

    codes.resize(count);
    for (usize i = 0; i < count; i++) {
        auto& c = codes[i];

        u32 size = file.ReadUint32();
        rstd_assert(size % 4 == 0);
        if (size % 4 != 0) return false;

        c.resize(size / 4);
        file.Read((char*)c.data(), size);
    }
    return true;
}

inline void SaveShaderToFile(std::span<const ShaderCode> codes, fs::IBinaryStreamW& file) {
    char nop[256] { '\0' };

    WriteShaderCacheVersion(file, 1);
    file.WriteUint32((u32)codes.size());
    for (const auto& c : codes) {
        u32 size = (u32)c.size() * 4;
        file.WriteUint32(size);
        file.Write((const char*)c.data(), size);
    }
    file.Write(nop, sizeof(nop));
}

} // namespace

std::string WPShaderParser::PreShaderSrc(fs::VFS& vfs, const std::string& src,
                                         WPShaderInfo*                       pWPShaderInfo,
                                         const std::vector<WPShaderTexInfo>& texinfos) {
    // Expand `#include "FILE"` in place: replace each include line with its
    // resolved content (recursively expanded). Preserves the include's
    // original position so a `struct Grid { ... }; #include "common.h"`
    // pattern doesn't end up nesting the include's functions inside the
    // struct body. ParseWPShader still runs over the resolved include text
    // (for `// [COMBO]` / `uniform NAME // {json}` extraction) and over the
    // user source (sans include directives).
    std::string newsrc;
    newsrc.reserve(src.size());
    std::string all_includes;

    usize                  cursor = 0;
    shader_lex::LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        shader_lex::Cursor c(src);
        c.SeekTo(w.LineStart());
        if (! c.MatchHashDirective("include")) continue;

        // Copy bytes up to this line, then splice in the recursively-expanded
        // include body. The newline after the directive stays as part of the
        // splice (we step the outer cursor to LineEnd).
        newsrc.append(src, cursor, w.LineStart() - cursor);
        std::string line     = src.substr(w.LineStart(), w.LineEnd() - w.LineStart());
        std::string expanded = LoadGlslInclude(vfs, line + "\n");
        newsrc.append(expanded);
        all_includes.append(expanded);
        cursor = w.LineEnd();
    }
    newsrc.append(src, cursor, std::string::npos);
    if (pWPShaderInfo != nullptr && pWPShaderInfo->normalize_tangent_space) {
        NormalizeExpandedShaderSource(newsrc);
    }

    ParseWPShader(all_includes, pWPShaderInfo, texinfos);
    ParseWPShader(newsrc, pWPShaderInfo, texinfos);

    return newsrc;
}

std::string WPShaderParser::PreShaderHeader(const std::string& src, const Combos& combos,
                                            ShaderType type) {
    const std::string user_src = NormalizeLocalMatrixMul(
        NormalizePackedAudioSpectrumAccess(UndefBeforeUserMacroDefines(src, "M_PI_2")));

    // All stages route through glslang's HLSL frontend.
    std::string pre;
    if (type == ShaderType::GEOMETRY) {
        pre = pre_shader_code_gs_hlsl;
    } else {
        pre = pre_shader_code;
        const char* tail =
            (type == ShaderType::FRAGMENT) ? pre_shader_tail_frag : pre_shader_tail_vert;
        if (auto pos = pre.find("__SHADER_TAIL__"); pos != std::string::npos) {
            pre.replace(pos, std::string_view("__SHADER_TAIL__").size(), tail);
        }
    }

    // If user shader defines its own `mod(...)` at file scope, gate out the
    // prologue's mod overloads to avoid redefinition errors. Substring scan
    // is good enough — function decls always start with one of these tokens
    // followed by a space and `mod(`.
    static constexpr std::string_view kModSentinels[] = {
        "\nfloat mod(", "\nfloat2 mod(", "\nfloat3 mod(", "\nfloat4 mod(",
        "\nvec2 mod(",  "\nvec3 mod(",   "\nvec4 mod(",
    };
    bool user_mod = false;
    for (auto needle : kModSentinels) {
        if (user_src.find(needle) != std::string::npos ||
            (user_src.size() >= needle.size() - 1 &&
             std::string_view(user_src).substr(0, needle.size() - 1) == needle.substr(1))) {
            user_mod = true;
            break;
        }
    }
    if (user_mod) {
        // Inject #define ahead of the prologue text so the #ifndef guard
        // around our `mod` overloads sees it during glslang preprocess.
        pre = "#define WW_USER_MOD 1\n" + pre;
    }

    std::string combo_defines;
    for (const auto& c : combos) {
        std::string cup(c.first);
        std::transform(c.first.begin(), c.first.end(), cup.begin(), ::toupper);
        if (c.second.empty()) {
            rstd_error("combo '{}' can't be empty", cup);
            continue;
        }
        combo_defines += "#define " + cup + " " + c.second + "\n";
    }

    // Combo `#define`s land before __SHADER_PLACEHOLD__ so they're visible
    // throughout the user source during the DXC -P pass. The placeholder
    // slot itself is filled by Finalprocessor *after* preprocessing, so
    // the synthesized cbuffer always sees combo references already
    // expanded to literal numbers (e.g. `g_Bones[BONECOUNT]` → `[4]`).
    if (auto pos = pre.find(SHADER_PLACEHOLD); pos != std::string::npos) {
        pre.insert(pos, combo_defines);
    } else {
        pre += combo_defines;
    }
    return pre + user_src;
}

void WPShaderParser::InitGlslang() { vulkan::InitProcess(); }
void WPShaderParser::FinalGlslang() { vulkan::FinalizeProcess(); }

namespace
{

// Serialize one CompileToSpv invocation as a JSON object. Captures the
// raw post-PreShaderSrc state (includes resolved, prologue not yet
// applied, regex extraction not yet run) so a replay through the full
// pipeline exercises every transform downstream.
Json BuildShaderRecord(std::string_view scene_id, std::span<const WPShaderUnit> units,
                       const WPShaderInfo* shader_info, std::span<const WPShaderTexInfo> texs) {
    auto stage_name = [](ShaderType s) -> const char* {
        switch (s) {
        case ShaderType::VERTEX: return "VERTEX";
        case ShaderType::FRAGMENT: return "FRAGMENT";
        case ShaderType::GEOMETRY: return "GEOMETRY";
        }
        return "UNKNOWN";
    };

    auto rec = rstd::json::Map::make();
    rec.insert(::alloc::string::String::make(rstd::cppstd::as_str("scene_id")),
               JsonFromStd(scene_id));

    auto js_stages = rstd::json::Array::make();
    for (const auto& u : units) {
        auto stage = rstd::json::Map::make();
        stage.insert(::alloc::string::String::make(rstd::cppstd::as_str("stage")),
                     JsonFromStd(stage_name(u.stage)));
        stage.insert(::alloc::string::String::make(rstd::cppstd::as_str("src")),
                     JsonFromStd(u.src));
        js_stages.push(Json::Object(rstd::move(stage)));
    }
    rec.insert(::alloc::string::String::make(rstd::cppstd::as_str("stages")),
               Json::Array(rstd::move(js_stages)));

    auto js_combos = rstd::json::Map::make();
    if (shader_info) {
        for (const auto& [k, v] : shader_info->combos)
            js_combos.insert(::alloc::string::String::make(rstd::cppstd::as_str(k)),
                             JsonFromStd(v));
    }
    rec.insert(::alloc::string::String::make(rstd::cppstd::as_str("combos")),
               Json::Object(rstd::move(js_combos)));

    auto js_texs = rstd::json::Array::make();
    for (const auto& t : texs) {
        auto compos = rstd::json::Array::make();
        for (bool enabled : t.composEnabled) compos.push(rstd::into<Json>(enabled));
        auto tex = rstd::json::Map::make();
        tex.insert(::alloc::string::String::make(rstd::cppstd::as_str("enabled")),
                   rstd::into<Json>(bool { t.enabled }));
        tex.insert(::alloc::string::String::make(rstd::cppstd::as_str("compos")),
                   Json::Array(rstd::move(compos)));
        js_texs.push(Json::Object(rstd::move(tex)));
    }
    rec.insert(::alloc::string::String::make(rstd::cppstd::as_str("tex_infos")),
               Json::Array(rstd::move(js_texs)));

    return Json::Object(rstd::move(rec));
}

// Appends one JSONL line to the shader-record path. O_APPEND is atomic for
// writes <= PIPE_BUF, which is more than enough for a single JSON line;
// concurrent recorders won't interleave.
void MaybeRecordCompile(std::string_view scene_id, std::span<const WPShaderUnit> units,
                        const WPShaderInfo* shader_info, std::span<const WPShaderTexInfo> texs) {
    const char* path = std::getenv("SCENERENDERER_SHADER_RECORD");
    if (! path || path[0] == '\0') return;
    Json rec  = BuildShaderRecord(scene_id, units, shader_info, texs);
    std::string line = Dump(rec);
    line.push_back('\n');
    if (FILE* f = std::fopen(path, "a")) {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    } else {
        rstd_warn("SCENERENDERER_SHADER_RECORD: cannot open '{}' for append", path);
    }
}

struct ProcessShaderCacheEntry {
    std::vector<WPShaderUnit> units;
    std::vector<ShaderCode>   codes;
    std::uint64_t             last_use { 0 };
};

std::mutex g_process_shader_cache_mutex;
std::unordered_map<std::string, ProcessShaderCacheEntry> g_process_shader_cache;
std::uint64_t g_process_shader_cache_clock { 0 };
constexpr std::size_t kMaxProcessShaderCacheEntries = 128;

std::string ProcessShaderCacheKey(std::span<const WPShaderUnit> units,
                                  const WPShaderInfo* shader_info,
                                  std::span<const WPShaderTexInfo> texs) {
    // BuildShaderRecord contains raw per-stage sources, combo values and
    // texture-component configuration. Salt it independently from the disk
    // SPIR-V format so either cache can evolve without accepting stale data.
    std::string material { "SceneRenderer-process-shader-v1\n" };
    material += Dump(BuildShaderRecord({}, units, shader_info, texs));
    return utils::genSha1(material);
}

bool LoadProcessShaderCache(std::string_view key, std::span<WPShaderUnit> units,
                            std::vector<ShaderCode>& codes) {
    std::lock_guard lock(g_process_shader_cache_mutex);
    auto it = g_process_shader_cache.find(std::string(key));
    if (it == g_process_shader_cache.end() || it->second.units.size() != units.size()) return false;
    std::copy(it->second.units.begin(), it->second.units.end(), units.begin());
    codes               = it->second.codes;
    it->second.last_use = ++g_process_shader_cache_clock;
    return true;
}

void SaveProcessShaderCache(std::string key, std::span<const WPShaderUnit> units,
                            const std::vector<ShaderCode>& codes) {
    std::lock_guard lock(g_process_shader_cache_mutex);
    if (g_process_shader_cache.size() >= kMaxProcessShaderCacheEntries &&
        ! g_process_shader_cache.contains(key)) {
        auto oldest = std::min_element(
            g_process_shader_cache.begin(),
            g_process_shader_cache.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.second.last_use < rhs.second.last_use; });
        if (oldest != g_process_shader_cache.end()) g_process_shader_cache.erase(oldest);
    }
    g_process_shader_cache.insert_or_assign(
        std::move(key),
        ProcessShaderCacheEntry {
            .units    = std::vector<WPShaderUnit>(units.begin(), units.end()),
            .codes    = codes,
            .last_use = ++g_process_shader_cache_clock,
        });
}

} // namespace

bool WPShaderParser::CompileToSpv(std::string_view scene_id, std::span<WPShaderUnit> units,
                                  std::vector<ShaderCode>& codes, fs::VFS& vfs,
                                  WPShaderInfo*                    shader_info,
                                  std::span<const WPShaderTexInfo> texs) {
    MaybeRecordCompile(scene_id, units, shader_info, texs);

    auto process_cache_key = ProcessShaderCacheKey(units, shader_info, texs);
    if (LoadProcessShaderCache(process_cache_key, units, codes)) return true;

    std::for_each(units.begin(), units.end(), [shader_info](auto& unit) {
        unit.src = Preprocessor(unit.src, unit.stage, shader_info->combos, unit.preprocess_info);
    });

    auto compile = [](std::span<WPShaderUnit> units, std::vector<ShaderCode>& codes) {
        // Build the cross-stage uniform union UP FRONT over ALL stages. Doing
        // this per-unit with just pre/next neighbours misses any uniform that
        // lives on a non-adjacent stage (e.g. FS-only `g_Brightness` not seen
        // by VS in a 3-stage VS→GS→FS chain), which results in different UBO
        // sizes per stage and the runtime allocating a buffer too small for
        // the longest stage.
        Map<std::string, std::string> uniforms_union;
        for (auto& unit : units) {
            for (const auto& [name, ty] : unit.preprocess_info.uniforms) {
                MergeUniform(uniforms_union, name, ty);
            }
        }

        std::vector<IODecl> varying_union;
        auto add_varying = [&](const IODecl& d) {
            if (d.storage == 'a') return;
            for (auto& e : varying_union) {
                if (e.name == d.name) {
                    e.type = WiderType(e.type, d.type);
                    return;
                }
            }
            varying_union.push_back(d);
        };
        auto add_varying_from_line = [&](const std::string& line) {
            if (auto d = ParseIODecl(line); d) add_varying(*d);
        };
        for (const auto& unit : units) {
            for (const auto& [name, line] : unit.preprocess_info.output) {
                (void)name;
                add_varying_from_line(line);
            }
            if (unit.stage != ShaderType::VERTEX) {
                for (const auto& [name, line] : unit.preprocess_info.input) {
                    (void)name;
                    add_varying_from_line(line);
                }
            }
        }

        std::vector<vulkan::ShaderCompUnit> vunits(units.size());
        for (usize i = 0; i < units.size(); i++) {
            auto&               unit     = units[i];
            auto&               vunit    = vunits[i];
            WPPreprocessorInfo* pre_info = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
            WPPreprocessorInfo* post_info =
                i + 1 < units.size() ? &units[i + 1].preprocess_info : nullptr;

            unit.src = Finalprocessor(unit, pre_info, post_info, &uniforms_union, &varying_union);

            vunit.src   = unit.src;
            vunit.stage = unit.stage;
            vunit.lang  = vulkan::SourceLang::Hlsl;
        }

        vulkan::ShaderCompOpt opt;
        opt.target   = vulkan::VulkanTarget::Vulkan_1_1;
        opt.optimize = false;

        std::vector<vulkan::Uni_ShaderSpv> spvs(units.size());

        if (! vulkan::CompileAndLinkShaderUnits(vunits, opt, spvs)) {
            return false;
        }

        codes.clear();
        for (auto& spv : spvs) {
            codes.emplace_back(std::move(spv->spirv));
        }
        return true;
    };

    bool has_cache_dir = vfs.IsMounted("cache");

    if (has_cache_dir) {
        std::string sha1            = GenSha1(units);
        std::string cache_file_path = GetCachePath(scene_id, sha1);

        if (vfs.Contains(cache_file_path)) {
            auto cache_file = vfs.Open(cache_file_path);
            if (! cache_file || ! ::LoadShaderFromFile(codes, *cache_file)) {
                rstd_error("load shader from \'{}\' failed", cache_file_path);
                return false;
            }
        } else {
            if (! compile(units, codes)) return false;
            if (auto cache_file = vfs.OpenW(cache_file_path); cache_file) {
                ::SaveShaderToFile(codes, *cache_file);
            }
        }
        SaveProcessShaderCache(std::move(process_cache_key), units, codes);
        return true;

    } else {
        if (! compile(units, codes)) return false;
        SaveProcessShaderCache(std::move(process_cache_key), units, codes);
        return true;
    }
}

namespace
{

WPShaderTexInfo ToWPShaderTexInfo(const SceneShaderTextureCompileInfo& info) {
    return WPShaderTexInfo {
        .enabled       = info.enabled,
        .composEnabled = info.components,
    };
}

SceneShaderTextureCompileInfo ToSceneShaderTextureCompileInfo(const WPShaderTexInfo& info) {
    return SceneShaderTextureCompileInfo {
        .enabled    = info.enabled,
        .components = info.composEnabled,
    };
}

std::vector<SceneShaderDefaultTexture> ToSceneShaderDefaultTextures(const WPShaderInfo& info) {
    std::vector<SceneShaderDefaultTexture> out;
    out.reserve(info.defTexs.size());
    for (const auto& [slot, texture] : info.defTexs) {
        out.push_back(SceneShaderDefaultTexture { .slot = slot, .texture = texture });
    }
    return out;
}

void MergeVariantFallbackMetadata(WPShaderInfo& info, const SceneShaderVariantDesc& desc) {
    for (const auto& [key, value] : desc.uniform_aliases) {
        if (! info.alias.contains(key)) info.alias[key] = value;
    }
    for (const auto& [key, value] : desc.default_uniforms) {
        if (! info.svs.contains(key)) info.svs[key] = value;
    }
    for (const auto& texture : desc.default_textures) {
        auto found = std::find_if(info.defTexs.begin(), info.defTexs.end(), [&](const auto& item) {
            return item.first == texture.slot;
        });
        if (found == info.defTexs.end()) info.defTexs.push_back({ texture.slot, texture.texture });
    }
}

} // namespace

void WPShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(
    SceneShaderVariantDesc& desc, std::span<const WPShaderUnit> units,
    std::span<const ShaderCode> codes) {
    for (usize i = 0; i < desc.stages.size() && i < units.size(); ++i) {
        desc.stages[i].active_texture_slots = units[i].preprocess_info.active_tex_slots;
        desc.stages[i].uniforms             = units[i].preprocess_info.uniforms;
        if (i < codes.size()) desc.stages[i].code_hash = SceneShaderStageCodeHash(codes[i]);
    }

    std::vector<vulkan::Uni_ShaderSpv> spvs;
    vulkan::ShaderReflected            reflected;
    if (! vulkan::GenReflect(codes, spvs, reflected)) return;

    struct BindingRecord {
        std::string name;
        uint32_t    binding { 0 };
        uint32_t    descriptor_type { 0 };
        uint32_t    descriptor_count { 0 };
        uint32_t    stage_flags { 0 };
    };
    struct UniformMemberRecord {
        std::string name;
        unsigned    offset { 0 };
        std::size_t size { 0 };
        std::size_t num { 0 };
    };
    struct UniformBlockRecord {
        std::string                      name;
        unsigned                         size { 0 };
        std::vector<UniformMemberRecord> members;
    };

    auto binding_less = [](const BindingRecord& lhs, const BindingRecord& rhs) {
        if (lhs.binding != rhs.binding) return lhs.binding < rhs.binding;
        return lhs.name < rhs.name;
    };
    auto member_less = [](const UniformMemberRecord& lhs, const UniformMemberRecord& rhs) {
        if (lhs.offset != rhs.offset) return lhs.offset < rhs.offset;
        return lhs.name < rhs.name;
    };
    auto block_less = [](const UniformBlockRecord& lhs, const UniformBlockRecord& rhs) {
        return lhs.name < rhs.name;
    };

    std::vector<BindingRecord> bindings;
    bindings.reserve(reflected.binding_map.size());
    for (const auto& [name, binding] : reflected.binding_map) {
        bindings.push_back(BindingRecord {
            .name             = name,
            .binding          = binding.binding,
            .descriptor_type  = static_cast<uint32_t>(binding.descriptorType),
            .descriptor_count = binding.descriptorCount,
            .stage_flags      = binding.stageFlags,
        });
    }
    std::sort(bindings.begin(), bindings.end(), binding_less);

    std::vector<UniformBlockRecord> blocks;
    blocks.reserve(reflected.blocks.size());
    for (const auto& block : reflected.blocks) {
        UniformBlockRecord record {
            .name = block.name,
            .size = block.size,
        };
        record.members.reserve(block.member_map.size());
        for (const auto& [name, member] : block.member_map) {
            record.members.push_back(UniformMemberRecord {
                .name   = name,
                .offset = member.offset,
                .size   = member.size,
                .num    = member.num,
            });
        }
        std::sort(record.members.begin(), record.members.end(), member_less);
        blocks.push_back(std::move(record));
    }
    std::sort(blocks.begin(), blocks.end(), block_less);

    std::size_t seed { 0 };
    utils::hash_combine(seed, bindings.size());
    for (const auto& binding : bindings) {
        utils::hash_combine(seed, binding.name);
        utils::hash_combine(seed, binding.binding);
        utils::hash_combine(seed, binding.descriptor_type);
        utils::hash_combine(seed, binding.descriptor_count);
        utils::hash_combine(seed, binding.stage_flags);
    }
    utils::hash_combine(seed, blocks.size());
    for (const auto& block : blocks) {
        utils::hash_combine(seed, block.name);
        utils::hash_combine(seed, block.size);
        utils::hash_combine(seed, block.members.size());
        for (const auto& member : block.members) {
            utils::hash_combine(seed, member.name);
            utils::hash_combine(seed, member.offset);
            utils::hash_combine(seed, member.size);
            utils::hash_combine(seed, member.num);
        }
    }
    desc.descriptor_layout_hash = seed;
}

CompileSceneShaderVariantResult
WPShaderParser::CompileSceneShaderVariant(const SceneShaderVariantDesc& desc, fs::VFS& vfs,
                                          const Combos& combos_override) {
    CompileSceneShaderVariantResult result;
    result.variant = desc;

    if (! desc.Valid()) {
        result.error = "invalid shader variant descriptor";
        return result;
    }

    result.tex_info.reserve(desc.texture_infos.size());
    for (const auto& texinfo : desc.texture_infos) {
        result.tex_info.push_back(ToWPShaderTexInfo(texinfo));
    }

    std::vector<WPShaderUnit> units;
    units.reserve(desc.stages.size());
    bool has_geometry_stage = false;
    for (const auto& stage : desc.stages) {
        if (stage.source.empty()) {
            result.error = "shader variant stage source is empty";
            return result;
        }
        has_geometry_stage = has_geometry_stage || stage.stage == ShaderType::GEOMETRY;
        units.push_back(WPShaderUnit {
            .stage           = stage.stage,
            .src             = stage.source,
            .preprocess_info = {},
        });
    }

    for (auto& unit : units) {
        unit.src = WPShaderParser::PreShaderSrc(vfs, unit.src, &result.info, result.tex_info);
    }

    for (const auto& [key, value] : desc.resolved_combos) {
        result.info.combos[key] = value;
    }
    for (const auto& [key, value] : combos_override) {
        result.info.combos[key]          = value;
        result.variant.input_combos[key] = value;
    }
    if (has_geometry_stage && ! result.info.combos.contains(std::string(WE_CB_GS_ENABLED))) {
        result.info.combos[std::string(WE_CB_GS_ENABLED)] = "1";
    }
    MergeVariantFallbackMetadata(result.info, desc);

    result.variant.resolved_combos         = result.info.combos;
    result.variant.uniform_aliases         = result.info.alias;
    result.variant.default_uniforms        = result.info.svs;
    result.variant.default_textures        = ToSceneShaderDefaultTextures(result.info);
    result.variant.geometry_shader_enabled = has_geometry_stage;
    result.variant.texture_infos.clear();
    result.variant.texture_infos.reserve(result.tex_info.size());
    for (const auto& texinfo : result.tex_info) {
        result.variant.texture_infos.push_back(ToSceneShaderTextureCompileInfo(texinfo));
    }

    std::vector<ShaderCode> spvs;
    InitGlslang();
    const bool ok = CompileToSpv(desc.scene_id,
                                 std::span<WPShaderUnit>(units.data(), units.size()),
                                 spvs,
                                 vfs,
                                 &result.info,
                                 result.tex_info);
    FinalGlslang();

    if (! ok) {
        result.error = "CompileToSpv failed";
        return result;
    }
    WPShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(result.variant, units, spvs);

    auto shader              = std::make_shared<SceneShader>();
    shader->name             = desc.shader_name;
    shader->codes            = std::move(spvs);
    shader->default_uniforms = result.info.svs;
    result.shader            = std::move(shader);
    result.ok                = true;
    return result;
}

CompileMaterialShaderResult WPShaderParser::CompileMaterialShader(const Json&      material_json,
                                                                  fs::VFS&         vfs,
                                                                  std::string_view scene_id,
                                                                  const Combos& combos_override) {
    CompileMaterialShaderResult r;

    wpscene::Material mat;
    if (! mat.FromJson(material_json)) {
        r.error = "Material::FromJson failed";
        return r;
    }
    r.shader_name = mat.shader;

    if (mat.shader.empty()) {
        r.error = "material has no shader name";
        return r;
    }

    const std::string shader_path = "/assets/shaders/" + mat.shader;
    std::string       vert_src    = fs::GetFileContent(vfs, shader_path + ".vert");
    std::string       frag_src    = fs::GetFileContent(vfs, shader_path + ".frag");
    std::string       geom_src;
    // genericropeparticle ships a geometry shader (.geom) that MoltenVK can't
    // lower (Metal has no geometry-shader stage); leave geom_src empty so this
    // helper never emits a GS variant. The runtime rope path expands segments
    // to quads on the CPU instead (see WPParticleRawGener::GenGLData).
    if (vert_src.empty() || frag_src.empty()) {
        r.error = "shader source missing: " + shader_path + ".{vert,frag}";
        return r;
    }

    // Texture info: enabled flag from non-empty material.textures.
    // composEnabled[3] would normally come from each .tex header
    // (extraHeader.compoN). Skipping the header parse keeps this entry
    // path lightweight; sprite-sheet / packed-channel materials may
    // accordingly compile a different variant than the production path.
    r.tex_info.reserve(mat.textures.size());
    for (const auto& t : mat.textures) {
        r.tex_info.push_back({ ! t.empty(), { false, false, false } });
    }

    // Combos: material's int combos -> string, then override wins.
    // Inject defaults that ParseImageObj always sets.
    for (const auto& kv : mat.combos) {
        r.info.combos[kv.first] = std::to_string(kv.second);
    }
    for (const auto& kv : combos_override) {
        r.info.combos[kv.first] = kv.second;
    }
    if (r.info.combos.find(std::string(WE_CB_BLENDMODE)) == r.info.combos.end())
        r.info.combos[std::string(WE_CB_BLENDMODE)] = "0";
    if (r.info.combos.find(std::string(WE_CB_BONECOUNT)) == r.info.combos.end())
        r.info.combos[std::string(WE_CB_BONECOUNT)] = "1";

    std::vector<WPShaderUnit> units;
    units.push_back({ ShaderType::VERTEX, std::move(vert_src), {} });
    if (! geom_src.empty()) {
        units.push_back({ ShaderType::GEOMETRY, std::move(geom_src), {} });
        r.info.combos[std::string(WE_CB_GS_ENABLED)] = "1";
    }
    units.push_back({ ShaderType::FRAGMENT, std::move(frag_src), {} });

    for (auto& u : units) {
        u.src = WPShaderParser::PreShaderSrc(vfs, u.src, &r.info, r.tex_info);
    }

    InitGlslang();
    const bool ok =
        WPShaderParser::CompileToSpv(scene_id,
                                     std::span<WPShaderUnit>(units.data(), units.size()),
                                     r.spvs,
                                     vfs,
                                     &r.info,
                                     r.tex_info);
    FinalGlslang();

    r.ok = ok;
    if (! ok) r.error = "CompileToSpv failed";
    return r;
}
