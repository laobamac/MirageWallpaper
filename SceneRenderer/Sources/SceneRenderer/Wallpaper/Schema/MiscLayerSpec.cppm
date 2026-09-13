export module sr.pkg.scene_obj:misc_object;
import rstd.cppstd;
import sr.fs;
import sr.json;
import :animation_layer;
export import :field_binding;
import :visibility_binding;
import :image_object;
import :scene_document;

// Object kinds beyond image/light/particle/sound: text overlays, .mdl
// model attachments, and editor camera markers. These exist only at the
// scene.json schema level; the renderer does not yet consume them, but the
// parser absorbs every observed top-level field so the data model stays
// schema-complete (drives SceneSchema.EveryParsedObjectKeyIsObserved).

export namespace sr::wpscene
{

// Text-overlay object (PKGV0005+). Discriminator: top-level `text` is
// non-null. The `text` and `font` fields appear in two shapes — plain
// string, or an object (e.g. `{"script": "..."}` for property-bound
// text). Both are captured verbatim as sr::Json so future consumers
// can decode either path without re-parsing.
struct TextObject {
    // Common positional/metadata (mirrors ImageObject prefix).
    std::int32_t         id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    bool                 visible { true };

    bool                      locktransforms { false };
    bool                      muteineditor { false };
    bool                      nointerpolation { false };
    std::uint32_t             parent { 0 };
    std::string               attachment;
    std::vector<std::int32_t> dependencies;
    sr::Json                 instance;
    FieldBindings             field_bindings;

    // Text-specific.
    sr::Json        text; // string | {script: ...} | {user: ..., value: ...}
    sr::Json        font; // string | {value: ...}
    float            pointsize { 12.0f };
    std::uint32_t    padding { 0 };
    std::string      horizontalalign;
    std::string      verticalalign;
    std::string      anchor;
    std::string      alignment { "center" };

    // Text-flow controls (PKGV0018+).
    std::uint32_t maxrows { 0 };
    float         maxwidth { 0.0f };
    bool          limitrows { false };
    bool          limitwidth { false };
    bool          limituseellipsis { false };

    VisibleUserBinding visible_user;
    std::string        visible_user_key;

    // user-property bindings for `text` / `pointsize` authored as
    // `{"user":"<key>", "value":...}`. Empty when the field is a plain
    // literal or a script binding. Drives live sidebar edits of custom text
    // and font size without a scene reload.
    UserValueBinding   text_user;
    std::string        text_user_key;
    UserValueBinding   pointsize_user;
    std::string        pointsize_user_key;
    UserValueBinding   color_user;
    std::string        color_user_key;
    UserValueBinding   alpha_user;
    std::string        alpha_user_key;
    UserValueBinding   scale_user;
    std::string        scale_user_key;
    UserValueBinding   maxwidth_user;
    std::string        maxwidth_user_key;

    // Visual/material overlap with image kind.
    std::array<float, 3>     color { 1.0f, 1.0f, 1.0f };
    float                    alpha { 1.0f };
    float                    brightness { 1.0f };
    int32_t                  colorBlendMode { 0 };
    std::array<float, 2>     size { 0.0f, 0.0f };
    bool                     perspective { false };
    bool                     reflected { true };
    bool                     copybackground { false };
    bool                     solid { false };
    bool                     opaquebackground { false };
    bool                     ledsource { false };
    std::array<float, 3>     backgroundcolor { 0.0f, 0.0f, 0.0f };
    float                    backgroundbrightness { 1.0f };
    std::vector<ImageEffect> effects;

    bool FromJson(const sr::Json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const sr::Json& json, fs::VFS& vfs, SceneVersion /*v*/) {
        sr::GetJsonValue(json, "id", id, false);
        sr::GetJsonValue(json, "name", name, false);
        sr::GetJsonValue(json, "origin", origin, false);
        sr::GetJsonValue(json, "scale", scale, false);
        sr::GetJsonValue(json, "angles", angles, false);
        sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);
        ReadVisibleProperty(json, visible, visible_user);
        visible_user_key = visible_user.name;
        sr::GetJsonValue(json, "locktransforms", locktransforms, false);
        sr::GetJsonValue(json, "muteineditor", muteineditor, false);
        sr::GetJsonValue(json, "nointerpolation", nointerpolation, false);
        sr::GetJsonValue(json, "parent", parent, false);
        sr::GetJsonValue(json, "attachment", attachment, false);
        sr::GetJsonValue(json, "dependencies", dependencies, false);
        if (auto value = json.get("instance"); value.is_some()) instance = (*value)->clone();
        if (auto value = json.get("text"); value.is_some()) text = (*value)->clone();
        if (auto value = json.get("font"); value.is_some()) font = (*value)->clone();

        sr::GetJsonValue(json, "pointsize", pointsize, false);
        ReadUserValueBinding(json, "text", text_user);
        text_user_key = text_user.name;
        ReadUserValueBinding(json, "pointsize", pointsize_user);
        pointsize_user_key = pointsize_user.name;
        ReadUserValueBinding(json, "color", color_user);
        color_user_key = color_user.name;
        ReadUserValueBinding(json, "alpha", alpha_user);
        alpha_user_key = alpha_user.name;
        ReadUserValueBinding(json, "scale", scale_user);
        scale_user_key = scale_user.name;
        ReadUserValueBinding(json, "maxwidth", maxwidth_user);
        maxwidth_user_key = maxwidth_user.name;
        sr::GetJsonValue(json, "padding", padding, false);
        sr::GetJsonValue(json, "horizontalalign", horizontalalign, false);
        sr::GetJsonValue(json, "verticalalign", verticalalign, false);
        sr::GetJsonValue(json, "anchor", anchor, false);
        sr::GetJsonValue(json, "alignment", alignment, false);
        sr::GetJsonValue(json, "maxrows", maxrows, false);
        sr::GetJsonValue(json, "maxwidth", maxwidth, false);
        sr::GetJsonValue(json, "limitrows", limitrows, false);
        sr::GetJsonValue(json, "limitwidth", limitwidth, false);
        sr::GetJsonValue(json, "limituseellipsis", limituseellipsis, false);
        sr::GetJsonValue(json, "color", color, false);
        sr::GetJsonValue(json, "alpha", alpha, false);
        sr::GetJsonValue(json, "brightness", brightness, false);
        sr::GetJsonValue(json, "colorBlendMode", colorBlendMode, false);
        sr::GetJsonValue(json, "size", size, false);
        sr::GetJsonValue(json, "perspective", perspective, false);
        sr::GetJsonValue(json, "reflected", reflected, false);
        sr::GetJsonValue(json, "copybackground", copybackground, false);
        sr::GetJsonValue(json, "solid", solid, false);
        sr::GetJsonValue(json, "opaquebackground", opaquebackground, false);
        sr::GetJsonValue(json, "ledsource", ledsource, false);
        sr::GetJsonValue(json, "backgroundcolor", backgroundcolor, false);
        sr::GetJsonValue(json, "backgroundbrightness", backgroundbrightness, false);
        if (auto effect_values = json.get("effects"); effect_values.is_some()) {
            auto array = (*effect_values)->as_array();
            if (array.is_some()) {
                for (const auto& jE : **array) {
                    ImageEffect wpeff;
                    wpeff.FromJson(jE, vfs);
                    effects.push_back(std::move(wpeff));
                }
            }
        }
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

// 3D model attachment (PKGV0001+). Discriminator: top-level `model` is a
// non-null string. WE links to a `.mdl` file under /assets and optionally
// names a sub-attachment to overlay.
struct ModelObject {
    std::int32_t         id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    bool                 visible { true };

    bool                      locktransforms { false };
    bool                      muteineditor { false };
    bool                      nointerpolation { false };
    std::uint32_t             parent { 0 };
    std::vector<std::int32_t> dependencies;
    sr::Json                 instance;
    FieldBindings             field_bindings;

    std::string   model;
    std::string   attachment;
    std::uint32_t skin { 0 };
    bool          perspective { false };
    bool          reflected { true };

    std::vector<WPPuppetLayer::AnimationLayer> puppet_layers;
    VisibleUserBinding                         visible_user;
    std::string                                visible_user_key;

    bool FromJson(const sr::Json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const sr::Json& json, fs::VFS&, SceneVersion /*v*/) {
        sr::GetJsonValue(json, "id", id, false);
        sr::GetJsonValue(json, "name", name, false);
        sr::GetJsonValue(json, "origin", origin, false);
        sr::GetJsonValue(json, "scale", scale, false);
        sr::GetJsonValue(json, "angles", angles, false);
        sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);
        ReadVisibleProperty(json, visible, visible_user);
        visible_user_key = visible_user.name;
        sr::GetJsonValue(json, "locktransforms", locktransforms, false);
        sr::GetJsonValue(json, "muteineditor", muteineditor, false);
        sr::GetJsonValue(json, "nointerpolation", nointerpolation, false);
        sr::GetJsonValue(json, "parent", parent, false);
        sr::GetJsonValue(json, "dependencies", dependencies, false);
        if (auto value = json.get("instance"); value.is_some()) instance = (*value)->clone();

        sr::GetJsonValue(json, "model", model, false);
        sr::GetJsonValue(json, "attachment", attachment, false);
        sr::GetJsonValue(json, "skin", skin, false);
        sr::GetJsonValue(json, "perspective", perspective, false);
        sr::GetJsonValue(json, "reflected", reflected, false);
        ReadPuppetAnimationLayers(json, puppet_layers);
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

// Editor camera marker (PKGV0020+). Discriminator: top-level `camera` is
// a non-null string. Carries camera animation paths and per-camera
// projection overrides.
struct CameraObject {
    std::int32_t         id { 0 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    bool                 visible { true };

    bool                      locktransforms { false };
    bool                      muteineditor { false };
    bool                      nointerpolation { false };
    std::uint32_t             parent { 0 };
    std::vector<std::int32_t> dependencies;
    sr::Json                 instance;
    FieldBindings             field_bindings;

    std::string camera; // camera name reference
    std::string path;   // animation path .json
    std::string queuemode;
    float       fov { 50.0f };
    float       zoom { 1.0f };
    bool        solid { false };
    bool        disablepropagation { false };

    VisibleUserBinding visible_user;
    std::string        visible_user_key;

    bool FromJson(const sr::Json& json, fs::VFS& vfs) {
        return FromJson(json, vfs, kSceneVersionUnknown);
    }
    bool FromJson(const sr::Json& json, fs::VFS&, SceneVersion /*v*/) {
        sr::GetJsonValue(json, "id", id, false);
        sr::GetJsonValue(json, "name", name, false);
        sr::GetJsonValue(json, "origin", origin, false);
        sr::GetJsonValue(json, "scale", scale, false);
        sr::GetJsonValue(json, "angles", angles, false);
        sr::GetJsonValue(json, "parallaxDepth", parallaxDepth, false);
        ReadVisibleProperty(json, visible, visible_user);
        visible_user_key = visible_user.name;
        sr::GetJsonValue(json, "locktransforms", locktransforms, false);
        sr::GetJsonValue(json, "muteineditor", muteineditor, false);
        sr::GetJsonValue(json, "nointerpolation", nointerpolation, false);
        sr::GetJsonValue(json, "parent", parent, false);
        sr::GetJsonValue(json, "dependencies", dependencies, false);
        if (auto value = json.get("instance"); value.is_some()) instance = (*value)->clone();

        sr::GetJsonValue(json, "camera", camera, false);
        sr::GetJsonValue(json, "path", path, false);
        sr::GetJsonValue(json, "queuemode", queuemode, false);
        sr::GetJsonValue(json, "fov", fov, false);
        sr::GetJsonValue(json, "zoom", zoom, false);
        sr::GetJsonValue(json, "solid", solid, false);
        sr::GetJsonValue(json, "disablepropagation", disablepropagation, false);
        AbsorbAllFieldBindings(json, field_bindings);
        return true;
    }
};

} // namespace sr::wpscene
