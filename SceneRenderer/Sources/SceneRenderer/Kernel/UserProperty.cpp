module;

module sr.user_property;

import rstd.cppstd;

namespace sr
{

namespace
{

Json MakeDescriptor(Json value) {
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make(rstd::cppstd::as_str("value")), std::move(value));
    return Json::Object(std::move(object));
}

std::string DescriptorType(const Json& descriptor) {
    auto type = descriptor.get("type");
    if (type.is_none()) return {};
    auto string = (**type).as_str();
    return string.is_some() ? rstd::cppstd::to_string(*string) : std::string {};
}

Json ParseWireValue(const Json& schema, const Json& value) {
    if (! value.is_string()) return value.clone();
    const auto type = DescriptorType(schema);
    if (type.empty() || type.compare("textinput") == 0) return value.clone();

    auto raw    = rstd::cppstd::as_string_view(*value.as_str());
    auto parsed = rstd::json::from_str(rstd::cppstd::as_str(raw), { .allow_comments = true });
    return parsed.is_ok() ? parsed.unwrap() : value.clone();
}

} // namespace

Json MakeUserPropertyWirePatch(std::string_view value) {
    return MakeDescriptor(JsonFromStd(value));
}

Json MergeUserPropertyDescriptor(const Json& schema, const Json& patch) {
    const Json* value = &patch;
    if (auto member = patch.get("value"); member.is_some()) value = &**member;
    const bool typed_patch = patch.get("type").is_some();

    Json descriptor   = schema.is_object() ? schema.clone() : MakeDescriptor(value->clone());
    auto object       = descriptor.as_object_mut();
    Json merged_value = typed_patch ? value->clone() : ParseWireValue(schema, *value);
    if (object.is_none()) return MakeDescriptor(std::move(merged_value));
    (*object)->insert(::alloc::string::String::make(rstd::cppstd::as_str("value")),
                      std::move(merged_value));
    return descriptor;
}

} // namespace sr
