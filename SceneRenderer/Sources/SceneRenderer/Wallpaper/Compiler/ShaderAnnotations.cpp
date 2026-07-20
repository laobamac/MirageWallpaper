module;

#include <rstd/macro.hpp>
#include "Utils/StringUtil.h"

module sr.pkg.parse;
import sr.core;
import sr.json;
import sr.scene;
import rstd.cppstd;
import rstd.log;
import :shader_lex;

// WE shader annotation collector. Walks the source line by line and pulls
// `// [COMBO]` / `uniform NAME; // {json}` annotations into WPShaderInfo.
// Collection is unconditional — #if/#endif dead-branch stripping is glslang's
// job downstream, and gating here creates chicken-and-egg cycles (texture
// combo flag inside `#if MASK == 1` depends on its own annotation).

namespace sr
{

namespace
{

using shader_lex::Cursor;
using shader_lex::LineWalker;

bool TryParseAnnotationJson(std::string_view source, Json& result) {
    auto parsed = rstd::json::from_str(rstd::cppstd::as_str(source));
    if (parsed.is_err()) return false;
    result = parsed.unwrap();
    return true;
}

bool CanStartNumberToken(std::string_view source, usize pos) {
    while (pos > 0) {
        --pos;
        char ch = source[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        return ch == '[' || ch == '{' || ch == ':' || ch == ',';
    }
    return true;
}

std::optional<std::string> NormalizeAnnotationNumbers(std::string_view source) {
    std::string out;
    out.reserve(source.size());

    bool in_string = false;
    bool escaped   = false;
    bool changed   = false;
    for (usize i = 0; i < source.size();) {
        char ch = source[i];
        if (in_string) {
            out.push_back(ch);
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            ++i;
            continue;
        }

        if (ch == '"') {
            in_string = true;
            out.push_back(ch);
            ++i;
            continue;
        }

        if ((ch == '-' || (ch >= '0' && ch <= '9')) && CanStartNumberToken(source, i)) {
            if (ch == '-') {
                if (i + 1 >= source.size() || source[i + 1] < '0' || source[i + 1] > '9') {
                    out.push_back(ch);
                    ++i;
                    continue;
                }
                out.push_back(ch);
                ++i;
            }
            while (i + 1 < source.size() && source[i] == '0' && source[i + 1] >= '0' &&
                   source[i + 1] <= '9') {
                changed = true;
                ++i;
            }
        }

        out.push_back(source[i]);
        ++i;
    }

    if (! changed) return std::nullopt;
    return out;
}

bool ParseAnnotationJson(std::string_view source, Json& result) {
    if (TryParseAnnotationJson(source, result)) return true;
    auto normalized = NormalizeAnnotationNumbers(source);
    return normalized && TryParseAnnotationJson(*normalized, result);
}

void HandleComboLine(WPShaderInfo* info, std::string_view line) {
    auto brace = line.find('{');
    if (brace == std::string_view::npos) return;
    Json j;
    if (! ParseAnnotationJson(line.substr(brace), j)) return;
    if (j.get("combo").is_none()) return;
    wpscene::WPCombo combo;
    combo.FromJson(j);
    if (combo.combo.empty()) return;
    info->combos[combo.combo] = std::to_string(combo.default_);
    info->combo_defs.push_back(std::move(combo));
}

void HandleUniformLine(WPShaderInfo* info, std::span<const WPShaderTexInfo> texinfos,
                       std::string_view line) {
    Cursor c(line);
    c.SkipHSpace();
    if (! c.MatchKeyword("uniform")) return;
    c.SkipHSpace();
    auto tn = shader_lex::ReadTypeName(c);
    if (! tn) return;
    c.SkipHSpace();
    (void)c.ReadArraySuffix();
    c.SkipHSpace();
    if (! c.MatchChar(';')) return;

    // Find the trailing `// {json}` blob.
    while (! c.Eof() && c.Peek() != '/') c.Advance();
    if (! c.MatchPunct("//")) return;
    while (! c.Eof() && c.Peek() != '{') c.Advance();
    if (c.Eof()) return;
    Json sv_json;
    if (! ParseAnnotationJson(line.substr(c.Pos()), sv_json)) return;

    auto name = tn->name;

    std::string material_key;
    GetJsonValue(sv_json, "material", material_key, false);
    if (! material_key.empty()) info->alias[material_key] = std::string(name);

    const bool is_tex   = name.compare(0, 9, "g_Texture") == 0;
    const idx  texcount = std::ssize(texinfos);

    if (is_tex) {
        wpscene::WPUniformTex wput;
        wput.FromJson(sv_json);
        i32 index { 0 };
        STRTONUM(name.substr(9), index);
        if (! wput.default_.empty()) {
            info->defTexs.push_back({ index, wput.default_ });
        }
        if (! wput.combo.empty()) {
            const bool enabled       = index < texcount && texinfos[(usize)index].enabled;
            info->combos[wput.combo] = enabled ? "1" : "0";
        }
        if (index < texcount && texinfos[(usize)index].enabled) {
            auto& compos = texinfos[(usize)index].composEnabled;
            usize num    = std::min(std::size(compos), std::size(wput.components));
            for (usize i = 0; i < num; i++) {
                if (compos[i]) info->combos[wput.components[i].combo] = "1";
            }
        }
        info->texture_uniforms.push_back(std::move(wput));
    } else {
        wpscene::WPUniformVar var;
        var.FromJson(sv_json, std::string(name));
        if (auto value = sv_json.get("default"); value.is_some()) {
            ShaderValue sv;
            if ((*value)->is_string()) {
                std::vector<float> values;
                GetJsonValue(**value, values);
                sv = std::span<const float>(values);
            } else if ((*value)->is_number()) {
                sv.setSize(1);
                GetJsonValue(**value, sv[0]);
            }
            info->svs[std::string(name)] = sv;
        }
        if (auto combo = sv_json.get("combo"); combo.is_some()) {
            std::string cname;
            GetJsonValue(sv_json, "combo", cname);
            if (! cname.empty()) info->combos[cname] = "1";
        }
        info->scalar_uniforms.push_back(std::move(var));
    }
}

} // namespace

void ParseWPShader(const std::string& src, WPShaderInfo* info,
                   const std::vector<WPShaderTexInfo>& texinfos_vec) {
    std::span<const WPShaderTexInfo> texinfos(texinfos_vec.data(), texinfos_vec.size());
    LineWalker                       w(src);
    for (; ! w.Done(); w.Step()) {
        auto line = w.Line();
        if (line.empty()) continue;
        // Helpers / forward decls above `void main()` are the annotated
        // region; the function body never carries new annotations.
        if (line.find("void main(") != std::string_view::npos) break;

        if (line.find("// [COMBO]") != std::string_view::npos) {
            HandleComboLine(info, line);
            continue;
        }
        // Cheap pre-check: only attempt the full keyword match if the trimmed
        // line could plausibly start with `uniform`.
        Cursor probe(line);
        probe.SkipHSpace();
        if (probe.Eof() || probe.Peek() != 'u') continue;
        HandleUniformLine(info, texinfos, line);
    }
}

} // namespace sr
