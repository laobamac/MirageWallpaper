#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import sr.text;
import sr.scene;
import sr.spec_texs;
import sr.fs;
import sr.pkg_fs;

namespace
{

bool Near(float actual, float expected, float epsilon = 0.001f) {
    if (std::abs(actual - expected) <= epsilon) return true;
    std::cerr << "expected " << expected << ", got " << actual << '\n';
    return false;
}

bool Check(bool value, std::string_view what) {
    if (value) return true;
    std::cerr << "failed: " << what << '\n';
    return false;
}

bool WriteFile(const std::filesystem::path& path, std::string_view value) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream output(path, std::ios::binary);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return output.good();
}

std::string BlobText(const sr::text::FontCache::ResolvedBlob& blob) {
    if (! blob.bytes) return {};
    return std::string(reinterpret_cast<const char*>(blob.bytes->data()), blob.bytes->size());
}

bool WritePackage(const std::filesystem::path& path,
                  const std::vector<std::pair<std::string, std::string>>& entries) {
    std::vector<std::uint8_t> bytes;
    const auto append_i32 = [&bytes](std::int32_t value) {
        const auto raw = static_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(raw >> shift));
    };
    const auto append_string = [&bytes, &append_i32](std::string_view value) {
        append_i32(static_cast<std::int32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    };

    append_string("PKGV0023");
    append_i32(static_cast<std::int32_t>(entries.size()));
    std::int32_t offset = 0;
    for (const auto& [name, value] : entries) {
        append_string(name);
        append_i32(offset);
        append_i32(static_cast<std::int32_t>(value.size()));
        offset += static_cast<std::int32_t>(value.size());
    }
    for (const auto& [_, value] : entries) bytes.insert(bytes.end(), value.begin(), value.end());

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

} // namespace

int main() {
    bool ok = true;

    int marker = 0;
    const auto root = std::filesystem::temp_directory_path() /
                      ("scenerenderer-font-resolution-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(&marker)));
    const auto shared_root = root / "shared";
    const auto package_path = root / "scene.pkg";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ok &= Check(WriteFile(shared_root / "fonts" / "library" / "三极萌萌简体.ttf", "shared"),
                "write shared font fixture");
    ok &= Check(WritePackage(package_path,
                             { { "fonts/exact.ttf", "exact" },
                               { "fonts/workshop/example/三极萌萌简体.ttf", "package" },
                               { "fonts/one/duplicate.ttf", "one" },
                               { "fonts/two/duplicate.ttf", "two" } }),
                "write package font fixture");

    sr::fs::VFS vfs;
    ok &= Check(vfs.Mount("/assets", sr::fs::CreatePhysicalFs(shared_root.string()), "assets"),
                "mount shared font fixture");
    auto package = sr::fs::WPPkgFs::CreatePkgFs(package_path.string(), true);
    ok &= Check(package && vfs.Mount("/assets", std::move(package)),
                "mount package font fixture");

    const auto exact = sr::text::FontCache::ResolveFont(vfs, "fonts/exact.ttf", false);
    ok &= Check(exact.source == "/assets/fonts/exact.ttf" && BlobText(exact) == "exact",
                "resolve exact package font path");

    const auto normalized =
        sr::text::FontCache::ResolveFont(vfs, "fonts\\exact.ttf", false);
    ok &= Check(normalized.source == "/assets/fonts/exact.ttf" && BlobText(normalized) == "exact",
                "resolve normalized package font path");

    const auto nested =
        sr::text::FontCache::ResolveFont(vfs, "fonts/三极萌萌简体.ttf", false);
    ok &= Check(nested.source == "/assets/fonts/workshop/example/三极萌萌简体.ttf" &&
                    BlobText(nested) == "package",
                "resolve unique package font basename");

    ok &= Check(! vfs.FindUniqueByBasenameInTopMount("/assets", "duplicate.ttf")
                       .has_value(),
                "reject ambiguous package font basename");
    std::filesystem::remove_all(root, ec);

    // Tabs in WE text are formatting artifacts and consume neither atlas
    // space nor layout width.
    auto font = sr::text::FontCache::ResolveSystemFont("systemfont_monospace");
    if (! font.bytes) {
        std::cerr << "failed to resolve a system font for tab regression\n";
        ok = false;
    } else {
        sr::text::FontCache cache;
        auto*               face = cache.GetFace(font.bytes, 64);
        const std::array<std::uint32_t, 1> tabs { '\t' };
        if (face == nullptr) {
            std::cerr << "failed to load system font for tab regression\n";
            ok = false;
        } else {
            face->Populate(tabs);
            const auto* tab = face->Lookup('\t');
            if (tab == nullptr || tab->pixel_w != 0 || tab->pixel_h != 0 ||
                ! Near(tab->advance_x, 0.0f)) {
                std::cerr << "tab unexpectedly produced a glyph or layout advance\n";
                ok = false;
            }
        }
    }

    // Workshop 3610728777: the dynamic clock has a 58 px logical frame but
    // only 35 px of visible ink. Bottom alignment must use 58; tight cropping
    // remains a separate compose concern.
    const sr::text::TextGeometryPolicy clock_policy {
        .frame_width        = 104.0f,
        .frame_height       = 58.0f,
        .dynamic            = true,
        .has_effect         = false,
        .preserve_text_bbox = false,
    };
    const sr::text::TextLayoutMetrics clock_metrics {
        .text_width       = 125.0f,
        .text_height      = 55.0f,
        .source_width     = 125.0f,
        .source_height    = 35.0f,
        .source_center_x  = 0.0f,
        .source_center_y  = 1.0f,
        .padding          = 0.0f,
        .source_centered  = true,
    };
    const auto clock_geometry = sr::text::ResolveTextGeometry(clock_policy, clock_metrics);
    const auto clock_anchor   = sr::text::ResolveTextAnchorPosition("center",
                                                                 "bottom",
                                                                 0.0f,
                                                                 -46.78589f,
                                                                 104.0f,
                                                                 58.0f,
                                                                 0.75f,
                                                                 0.75f,
                                                                 clock_metrics.text_width,
                                                                 clock_metrics.text_height);

    ok &= Near(clock_geometry.draw_height, 35.0f);
    ok &= Near(clock_geometry.draw_offset_y, 1.0f);
    // Bottom-aligned: the 55 px line box sits flush on the frame's bottom
    // edge, so the anchor rises by half the line box, not half the frame.
    ok &= Near(clock_anchor[1], -26.16089f);

    // The neighbouring date is centre-aligned. Anchoring by the line box keeps
    // the final parent-scaled ink gap positive; anchoring by the 35 px ink box
    // instead would make it negative (overlap).
    const auto date_anchor = sr::text::ResolveTextAnchorPosition("center",
                                                                "center",
                                                                0.0f,
                                                                -54.06921f,
                                                                66.0f,
                                                                24.0f,
                                                                1.25f,
                                                                1.25f,
                                                                40.0f,
                                                                20.0f);
    constexpr float parent_scale       = 1.4f;
    constexpr float date_source_center = 0.5f;
    constexpr float date_draw_height   = 15.0f;
    const float clock_visual_center =
        clock_anchor[1] + clock_geometry.draw_offset_y * 0.75f;
    const float date_visual_center = date_anchor[1] + date_source_center * 1.25f;
    const float centre_distance =
        std::abs(clock_visual_center - date_visual_center) * parent_scale;
    const float half_heights =
        (clock_geometry.draw_height * 0.75f + date_draw_height * 1.25f) *
        parent_scale * 0.5f;
    ok &= Near(centre_distance - half_heights, 7.74664f, 0.002f);

    // An effect layer retains the font-baseline source position. Geometry
    // resolution must keep the authored 533x238 frame intact while ensuring
    // asymmetric ink remains inside its RT.
    const sr::text::TextGeometryPolicy day_policy {
        .frame_width        = 533.0f,
        .frame_height       = 238.0f,
        .dynamic            = false,
        .has_effect         = true,
        .preserve_text_bbox = false,
    };
    const sr::text::TextLayoutMetrics day_metrics {
        .text_width       = 370.0f,
        .text_height      = 165.0f,
        .source_width     = 370.0f,
        .source_height    = 144.0f,
        .source_center_x  = 0.0f,
        .source_center_y  = -11.5f,
        .padding          = 32.0f,
        .source_centered  = false,
    };
    const auto day_geometry = sr::text::ResolveTextGeometry(day_policy, day_metrics);
    ok &= Near(day_geometry.rt_width, 533.0f);
    ok &= Near(day_geometry.rt_height, 238.0f);
    ok &= Near(day_geometry.draw_width, 533.0f);
    ok &= Near(day_geometry.draw_height, 238.0f);

    // A preserved logical bbox (opaque/copy-background text) may be wider
    // than its visible ink. Its RT and UV window must cover that full bbox so
    // the background is neither clipped nor stretched.
    const sr::text::TextGeometryPolicy background_policy {
        .frame_width        = 80.0f,
        .frame_height       = 20.0f,
        .dynamic            = false,
        .has_effect         = false,
        .preserve_text_bbox = true,
    };
    const sr::text::TextLayoutMetrics background_metrics {
        .text_width       = 80.0f,
        .text_height      = 20.0f,
        .source_width     = 60.0f,
        .source_height    = 10.0f,
        .source_center_x  = 5.0f,
        .source_center_y  = 0.0f,
        .padding          = 2.0f,
        .source_centered  = false,
    };
    const auto background_geometry =
        sr::text::ResolveTextGeometry(background_policy, background_metrics);
    ok &= Near(background_geometry.rt_width, 84.0f);
    ok &= Near(background_geometry.rt_height, 24.0f);
    ok &= Near(background_geometry.draw_width, 84.0f);
    ok &= Near(background_geometry.draw_height, 24.0f);
    ok &= Near(background_geometry.uv_source_width, 84.0f);
    ok &= Near(background_geometry.uv_source_height, 24.0f);

    if (font.bytes) {
        sr::text::FontCache large_cache;
        auto* large_face = large_cache.GetFace(font.bytes, 128);
        if (large_face == nullptr) {
            std::cerr << "failed to load exact-256-raster font\n";
            ok = false;
        } else {
            std::vector<std::uint32_t> printable;
            for (std::uint32_t cp = 33; cp <= 126; ++cp) printable.push_back(cp);
            large_face->Populate(printable);
            ok &= large_face->Metrics().atlas_w == 4096;
            for (std::uint32_t cp : printable) {
                const auto* glyph = large_face->Lookup(cp);
                if (glyph == nullptr || glyph->pixel_w == 0 || glyph->pixel_h == 0 ||
                    (glyph->atlas_x == 0 && glyph->atlas_y == 0)) {
                    std::cerr << "printable glyph did not receive an atlas slot: " << cp << '\n';
                    ok = false;
                    break;
                }
            }
        }

        sr::text::FontCache style_cache;
        auto* style_face = style_cache.GetFace(font.bytes, 32);
        if (style_face == nullptr) {
            std::cerr << "failed to load text style font\n";
            ok = false;
        } else {
            const std::array<std::uint32_t, 1> glyphs { 'A' };
            style_face->Populate(glyphs);
            auto mesh = std::make_shared<sr::SceneMesh>(true);
            mesh->AddVertexArray(sr::SceneVertexArray(
                sr::MakeAttrSet({ sr::VAttr::Position, sr::VAttr::TexCoord, sr::VAttr::Color }), 4));
            mesh->AddIndexArray(sr::SceneIndexArray(6));
            sr::text::TextLayoutStyle style;
            style.color      = { 0.8f, 0.6f, 0.4f };
            style.alpha      = 0.5f;
            style.brightness = 0.25f;
            sr::text::TextLayouter layouter(style_face, mesh, style, 1);
            layouter.SetText("A");

            const auto& vertex = mesh->GetVertexArray(0);
            const auto attrs = vertex.GetAttrOffsetMap();
            const auto color = attrs.find("a_Color");
            if (color == attrs.end()) {
                std::cerr << "text color vertex attribute missing\n";
                ok = false;
            } else {
                const auto offset = color->second.offset / sizeof(float);
                ok &= Near(vertex.Data()[offset + 0], 0.2f);
                ok &= Near(vertex.Data()[offset + 1], 0.15f);
                ok &= Near(vertex.Data()[offset + 2], 0.1f);
                ok &= Near(vertex.Data()[offset + 3], 0.5f);
                layouter.SetAlpha(0.2f);
                ok &= Near(vertex.Data()[offset + 3], 0.2f);
            }

            auto background_mesh = std::make_shared<sr::SceneMesh>(true);
            background_mesh->AddVertexArray(sr::SceneVertexArray(
                sr::MakeAttrSet({ sr::VAttr::Position, sr::VAttr::TexCoord, sr::VAttr::Color }), 8));
            background_mesh->AddIndexArray(sr::SceneIndexArray(12));
            sr::text::TextLayoutStyle background_style;
            background_style.alpha            = 0.5f;
            background_style.opaquebackground = true;
            background_style.background_color = { 0.4f, 0.6f, 0.8f };
            sr::text::TextLayouter background_layouter(
                style_face, background_mesh, background_style, 2);
            background_layouter.SetText("A");

            const auto& background_vertex = background_mesh->GetVertexArray(0);
            const auto background_attrs = background_vertex.GetAttrOffsetMap();
            const auto background_color = background_attrs.find("a_Color");
            if (background_color == background_attrs.end()) {
                std::cerr << "text background color vertex attribute missing\n";
                ok = false;
            } else {
                const auto background_offset = background_color->second.offset / sizeof(float);
                const auto glyph_offset = background_offset + 4 * background_vertex.OneSize();
                ok &= Near(background_vertex.Data()[background_offset + 3], 0.5f);
                ok &= Near(background_vertex.Data()[glyph_offset + 3], 0.5f);
                background_layouter.SetAlpha(0.2f);
                ok &= Near(background_vertex.Data()[background_offset + 3], 0.2f);
                ok &= Near(background_vertex.Data()[glyph_offset + 3], 0.2f);
            }
        }
    }

    return ok ? 0 : 1;
}
