module;

#include <rstd/macro.hpp>
#include "Utils/StringUtil.h"

module sr.pkg.parse;
import nlohmann.json;
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

void HandleComboLine(WPShaderInfo* info, std::string_view line) {
    auto brace = line.find('{');
    if (brace == std::string_view::npos) return;
    nlohmann::json j;
    if (! ParseJson(std::string(line.substr(brace)), j)) return;
    if (! j.contains("combo")) return;
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
    nlohmann::json sv_json;
    if (! ParseJson(std::string(line.substr(c.Pos())), sv_json)) return;

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
    } else {
        wpscene::WPUniformVar var;
        var.FromJson(sv_json, std::string(name));
        if (sv_json.contains("default")) {
            const auto& value = sv_json.at("default");
            ShaderValue sv;
            if (value.is_string()) {
                std::vector<float> v;
                GetJsonValue(value, v);
                sv = std::span<const float>(v);
            } else if (value.is_number()) {
                sv.setSize(1);
                GetJsonValue(value, sv[0]);
            }
            info->svs[std::string(name)] = sv;
        }
        if (sv_json.contains("combo")) {
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
