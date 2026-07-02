module;

export module sr.pkg.scene_obj:animation_layer;
import rstd.cppstd;
import sr.json;
export import sr.pkg.puppet;

export namespace sr::wpscene
{

inline void ReadPuppetAnimationLayers(const nlohmann::json&                       json,
                                      std::vector<WPPuppetLayer::AnimationLayer>& out) {
    if (! json.contains("animationlayers")) return;
    for (const auto& jLayer : json.at("animationlayers")) {
        WPPuppetLayer::AnimationLayer layer;
        sr::GetJsonValue(jLayer, "animation", layer.id);
        sr::GetJsonValue(jLayer, "blend", layer.blend);
        sr::GetJsonValue(jLayer, "rate", layer.rate);
        sr::GetJsonValue(jLayer, "visible", layer.visible, false);
        sr::GetJsonValue(jLayer, "id", layer.layer_id, false);
        sr::GetJsonValue(jLayer, "name", layer.name, false);
        sr::GetJsonValue(jLayer, "additive", layer.additive, false);
        sr::GetJsonValue(jLayer, "blendin", layer.blendin, false);
        sr::GetJsonValue(jLayer, "blendout", layer.blendout, false);
        sr::GetJsonValue(jLayer, "blendtime", layer.blendtime, false);
        out.push_back(std::move(layer));
    }
}

} // namespace sr::wpscene
