module;

#include <array>
#include <rstd/macro.hpp>

module sr.json;
import rstd.cppstd;
import rstd.json;
import rstd.log;

namespace sr
{

namespace
{

template<typename>
struct JsonArrayTarget {
    static constexpr bool enabled = false;
};

template<typename T>
struct JsonArrayTarget<std::vector<T>> {
    static constexpr bool enabled = true;
    using value_type              = T;
};

template<typename T, std::size_t N>
struct JsonArrayTarget<std::array<T, N>> {
    static constexpr bool enabled = true;
    using value_type              = T;
};

struct WrongJsonType : std::exception {
    auto what() const noexcept -> const char* override { return "Wrong json value type"; }
};

struct WrongArraySize : std::exception {
    auto what() const noexcept -> const char* override { return "Wrong size of the array"; }
};

auto InitialJsonValue(const Json& json) -> const Json& {
    if (auto value = json.get("value"); value.is_some()) return **value;
    return json;
}

template<typename T>
auto ParseNumber(std::string_view value) -> T {
    std::string text { value };
    if constexpr (std::is_same_v<T, float>) {
        return std::stof(text);
    } else if constexpr (std::is_same_v<T, double>) {
        return std::stod(text);
    } else if constexpr (std::is_signed_v<T>) {
        return static_cast<T>(std::stoll(text));
    } else {
        return static_cast<T>(std::stoull(text));
    }
}

template<typename T>
auto ConvertNumber(const Json& json) -> T {
    auto number = json.as_number();
    if (number.is_none()) throw WrongJsonType {};
    if ((*number)->is_f64()) return static_cast<T>(*(*number)->as_f64());
    if ((*number)->is_u64()) return static_cast<T>(*(*number)->as_u64());
    return static_cast<T>(*(*number)->as_i64());
}

template<typename T>
auto ConvertArray(std::string_view value, std::vector<T>& target) -> bool {
    std::vector<std::string_view> parts;
    while (true) {
        const auto delimiter = value.find(' ');
        if (delimiter == std::string_view::npos) {
            parts.push_back(value);
            break;
        }
        parts.push_back(value.substr(0, delimiter));
        value.remove_prefix(delimiter + 1);
    }
    if (target.size() < parts.size()) target.resize(parts.size());
    std::transform(parts.begin(), parts.end(), target.begin(), [](std::string_view part) {
        return ParseNumber<T>(part);
    });
    return true;
}

template<typename T, std::size_t N>
auto ConvertArray(std::string_view value, std::array<T, N>& target) -> bool {
    std::array<std::string_view, N> parts;
    std::size_t                     count = 0;
    while (true) {
        const auto delimiter = value.find(' ');
        if (count == N) throw WrongArraySize {};
        if (delimiter == std::string_view::npos) {
            parts[count++] = value;
            break;
        }
        parts[count++] = value.substr(0, delimiter);
        value.remove_prefix(delimiter + 1);
    }
    if (count != N) throw WrongArraySize {};
    std::transform(parts.begin(), parts.end(), target.begin(), [](std::string_view part) {
        return ParseNumber<T>(part);
    });
    return true;
}

template<typename T>
auto ReadJsonValue(const Json& json, T& value) -> bool {
    const auto& input = InitialJsonValue(json);
    if constexpr (JsonArrayTarget<T>::enabled) {
        using Value = typename JsonArrayTarget<T>::value_type;
        if (input.is_number()) {
            value = { ConvertNumber<Value>(input) };
            return true;
        }
        auto string = input.as_str();
        if (string.is_none()) throw WrongJsonType {};
        return ConvertArray(rstd::cppstd::as_string_view(*string), value);
    } else if constexpr (std::is_same_v<T, bool>) {
        auto boolean = input.as_bool();
        if (boolean.is_none()) throw WrongJsonType {};
        value = *boolean;
        return true;
    } else if constexpr (std::is_arithmetic_v<T>) {
        auto boolean = input.as_bool();
        value        = boolean.is_some() ? static_cast<T>(*boolean) : ConvertNumber<T>(input);
        return true;
    } else if constexpr (std::is_same_v<T, std::string>) {
        auto string = input.as_str();
        if (string.is_none()) throw WrongJsonType {};
        value = rstd::cppstd::to_string(*string);
        return true;
    }
}

template<typename T>
auto ReadJsonValue(const Json& json, T& value, const char* name, std::source_location loc) -> bool {
    std::string name_info;
    if (name != nullptr) name_info = std::string("(key: ") + name + ")";
    try {
        return ReadJsonValue(json, value);
    } catch (const WrongJsonType& error) {
        rstd_info("{} {} at {} {}:{}\n{}",
                  std::string_view(error.what()),
                  name_info,
                  std::string_view(loc.function_name()),
                  std::string_view(loc.file_name()),
                  loc.line(),
                  Dump(json, 4));
    } catch (const std::invalid_argument& error) {
        rstd_error("{} {} at {} {}:{}",
                   std::string_view(error.what()),
                   name_info,
                   std::string_view(loc.function_name()),
                   std::string_view(loc.file_name()),
                   loc.line());
    } catch (const std::out_of_range& error) {
        rstd_error("{} {} at {} {}:{}",
                   std::string_view(error.what()),
                   name_info,
                   std::string_view(loc.function_name()),
                   std::string_view(loc.file_name()),
                   loc.line());
    } catch (const WrongArraySize& error) {
        rstd_error("{} {} at {} {}:{}",
                   std::string_view(error.what()),
                   name_info,
                   std::string_view(loc.function_name()),
                   std::string_view(loc.file_name()),
                   loc.line());
    }
    return false;
}

} // namespace

template<typename T>
typename JsonTemplateTypeCheck<T>::type GetJsonValue(const Json& json, T& value,
                                                     std::source_location loc) {
    return ReadJsonValue(json, value, nullptr, loc);
}

template<typename T>
typename JsonTemplateTypeCheck<T>::type GetJsonValue(const Json& json, std::string_view name_view,
                                                     T& value, bool warn,
                                                     std::source_location loc) {
    auto member = json.get(name_view);
    if (member.is_none()) {
        if (warn)
            rstd_info("read json \"{}\" not a key at {}({}:{})",
                      name_view,
                      std::string_view(loc.function_name()),
                      std::string_view(loc.file_name()),
                      loc.line());
        return false;
    }
    if ((*member)->is_null()) {
        if (warn)
            rstd_info("read json \"{}\" is null at {}({}:{})",
                      name_view,
                      std::string_view(loc.function_name()),
                      std::string_view(loc.file_name()),
                      loc.line());
        return false;
    }
    std::string name { name_view };
    return ReadJsonValue(**member, value, name.c_str(), loc);
}

#define OWE_IMPL_GET_JSON(TYPE)                                    \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>( \
        const Json&, TYPE&, std::source_location);                 \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>( \
        const Json&, std::string_view, TYPE&, bool, std::source_location)

OWE_IMPL_GET_JSON(bool);
OWE_IMPL_GET_JSON(std::int32_t);
OWE_IMPL_GET_JSON(std::uint32_t);
OWE_IMPL_GET_JSON(float);
OWE_IMPL_GET_JSON(double);
OWE_IMPL_GET_JSON(std::string);
OWE_IMPL_GET_JSON(std::vector<float>);
OWE_IMPL_GET_JSON(std::vector<std::int32_t>);

using IntArray3   = std::array<int, 3>;
using FloatArray2 = std::array<float, 2>;
using FloatArray3 = std::array<float, 3>;
OWE_IMPL_GET_JSON(IntArray3);
OWE_IMPL_GET_JSON(FloatArray2);
OWE_IMPL_GET_JSON(FloatArray3);

#undef OWE_IMPL_GET_JSON

auto ParseJson(std::string_view source, rstd::json::ParseOptions options)
    -> rstd::json::ParseResult {
    return rstd::json::from_str(rstd::cppstd::as_str(source), options);
}

auto Dump(const Json& value, std::optional<std::size_t> indent) -> std::string {
    auto options = rstd::json::FormatOptions {};
    if (indent) {
        options.pretty = true;
        options.indent = *indent;
    }
    return rstd::cppstd::to_string(rstd::json::to_string(value, options));
}

} // namespace sr
