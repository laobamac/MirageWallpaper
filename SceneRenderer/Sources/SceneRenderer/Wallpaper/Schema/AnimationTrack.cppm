module;

export module sr.pkg.scene_obj:animation_layer;
import rstd.cppstd;
import sr.json;
export import sr.pkg.puppet;

export namespace sr::wpscene
{

inline void ReadPuppetAnimationLayers(const sr::Json&                            json,
                                      std::vector<WPPuppetLayer::AnimationLayer>& out) {
    auto layers = json.get("animationlayers");
    if (layers.is_none()) return;
    auto array = (*layers)->as_array();
    if (array.is_none()) return;
    for (const auto& jLayer : **array) {
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
        if (auto visible = jLayer.get("visible"); visible.is_some() && (*visible)->is_object()) {
            if (auto user = (*visible)->get("user"); user.is_some()) {
                if ((*user)->is_string()) {
                    layer.visible_user = rstd::cppstd::to_string(*(*user)->as_str());
                } else if ((*user)->is_object()) {
                    if (auto name = (*user)->get("name"); name.is_some() && (*name)->is_string())
                        layer.visible_user = rstd::cppstd::to_string(*(*name)->as_str());
                    if (auto condition = (*user)->get("condition"); condition.is_some()) {
                        layer.visible_condition = std::make_shared<sr::Json>((*condition)->clone());
                        layer.visible_has_condition = true;
                    }
                }
            }
        }
        out.push_back(std::move(layer));
    }
}

} // namespace sr::wpscene
