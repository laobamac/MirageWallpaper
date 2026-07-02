export module sr.json;
import rstd.cppstd;
export import nlohmann.json;

export namespace sr
{

template<typename T>
struct JsonTemplateTypeCheck {
    using type = bool;
    static_assert(! std::is_const_v<T>, "GetJsonValue need a non const value");
};

template<typename T>
typename sr::JsonTemplateTypeCheck<T>::type
GetJsonValue(const nlohmann::json& json, T& value,
             std::source_location loc = std::source_location::current());

template<typename T>
typename sr::JsonTemplateTypeCheck<T>::type
GetJsonValue(const nlohmann::json& json, std::string_view name, T& value, bool warn = true,
             std::source_location loc = std::source_location::current());

bool ParseJson(std::string_view source, nlohmann::json& result,
               std::source_location loc = std::source_location::current());

} // namespace sr
