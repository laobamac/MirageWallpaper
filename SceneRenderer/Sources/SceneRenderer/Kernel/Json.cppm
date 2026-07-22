module;
#include <array>

export module sr.json;
import rstd.cppstd;
export import rstd.json;

export namespace sr
{

using Json = rstd::json::Value;

template<typename T>
struct JsonTemplateTypeCheck {
    using type = bool;
    static_assert(! std::is_const_v<T>, "GetJsonValue need a non const value");
};

template<typename T>
typename JsonTemplateTypeCheck<T>::type
GetJsonValue(const Json& json, T& value,
             std::source_location loc = std::source_location::current());

template<typename T>
typename JsonTemplateTypeCheck<T>::type
GetJsonValue(const Json& json, std::string_view name, T& value, bool warn = true,
             std::source_location loc = std::source_location::current());

auto ParseJson(std::string_view source, rstd::json::ParseOptions options = {})
    -> rstd::json::ParseResult;
auto Dump(const Json& value, std::optional<std::size_t> indent = std::nullopt) -> std::string;

inline auto JsonFromStd(std::string_view value) -> Json {
    return rstd::into<Json>(::alloc::string::String::make(rstd::cppstd::as_str(value)));
}

} // namespace sr
