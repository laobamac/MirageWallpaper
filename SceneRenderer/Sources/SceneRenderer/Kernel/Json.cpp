module;

#include <rstd/macro.hpp>
#include <type_traits>

module sr.json;
import rstd.log;
import rstd.cppstd;
import nlohmann.json;

namespace sr
{

bool ParseJson(std::string_view source, nlohmann::json& result, std::source_location loc) {
    try {
        result = nlohmann::json::parse(source);
    } catch (nlohmann::json::parse_error& e) {
        rstd_error("parse json({}), {} at {}:{}",
                   std::string_view(loc.function_name()),
                   std::string_view(e.what()),
                   std::string_view(loc.file_name()),
                   loc.line());
        return false;
    }
    return true;
}

namespace
{

template<typename>
struct JsonArrayTarget {};

template<typename T>
struct JsonArrayTarget<std::vector<T>> {
    using type       = std::vector<T>;
    using value_type = T;
};

template<typename T, std::size_t N>
struct JsonArrayTarget<std::array<T, N>> {
    using type       = std::array<T, N>;
    using value_type = T;
};

struct WrongArraySizeExp : public std::exception {
    const char* what() const noexcept override { return "Wrong size of the array"; }
};

template<typename T>
T ParseNumber(std::string_view value) {
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

std::vector<std::string_view> Split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    while (true) {
        std::size_t pos = value.find(delimiter);
        if (pos == std::string_view::npos) {
            result.push_back(value);
            return result;
        }
        result.push_back(value.substr(0, pos));
        value.remove_prefix(pos + 1);
    }
}

template<typename T>
bool ConvertArray(std::string_view value, std::vector<T>& target) {
    const auto parts = Split(value, ' ');
    if (target.size() < parts.size()) target.resize(parts.size());
    std::transform(parts.begin(), parts.end(), target.begin(), [](std::string_view part) {
        return ParseNumber<T>(part);
    });
    return true;
}

template<typename T, std::size_t N>
bool ConvertArray(std::string_view value, std::array<T, N>& target) {
    const auto parts = Split(value, ' ');
    if (parts.size() != N) throw WrongArraySizeExp();
    std::transform(parts.begin(), parts.end(), target.begin(), [](std::string_view part) {
        return ParseNumber<T>(part);
    });
    return true;
}

} // namespace

template<typename T>
inline bool _GetJsonValue(const nlohmann::json& json, typename JsonArrayTarget<T>::type& value) {
    using Tv          = typename JsonArrayTarget<T>::value_type;
    const auto* pjson = &json;
    if (json.contains("value")) pjson = &json.at("value");
    const auto& njson = *pjson;
    if (njson.is_number()) {
        value = { njson.get<Tv>() };
        return true;
    } else {
        std::string strvalue;
        strvalue = njson.get<std::string>();
        return ConvertArray(strvalue, value);
    }
}

template<typename T>
inline bool _GetJsonValue(const nlohmann::json& json, T& value) {
    if (json.contains("value"))
        value = json.at("value").get<T>();
    else
        value = json.get<T>();
    return true;
}

template<typename T>
inline bool _GetJsonValue(const nlohmann::json& json, T& value, const char* name,
                          std::source_location loc) {
    using njson = nlohmann::json;
    std::string nameinfo;
    if (name != nullptr) nameinfo = std::string("(key: ") + name + ")";
    try {
        return _GetJsonValue<T>(json, value);
    } catch (const njson::type_error& e) {
        rstd_info("{} {} at {} {}:{}\n{}",
                  std::string_view(e.what()),
                  nameinfo,
                  std::string_view(loc.function_name()),
                  std::string_view(loc.file_name()),
                  loc.line(),
                  json.dump(4));
    } catch (const std::invalid_argument& e) {
        rstd_error("{} {} at {} {}:{}",
                   std::string_view(e.what()),
                   nameinfo,
                   std::string_view(loc.function_name()),
                   std::string_view(loc.file_name()),
                   loc.line());
    } catch (const std::out_of_range& e) {
        rstd_error("{} {} at {} {}:{}",
                   std::string_view(e.what()),
                   nameinfo,
                   std::string_view(loc.function_name()),
                   std::string_view(loc.file_name()),
                   loc.line());
    } catch (const WrongArraySizeExp& e) {
        rstd_error("{} {} at {} {}:{}",
                   std::string_view(e.what()),
                   nameinfo,
                   std::string_view(loc.function_name()),
                   std::string_view(loc.file_name()),
                   loc.line());
    }
    return false;
}

template<typename T>
typename JsonTemplateTypeCheck<T>::type GetJsonValue(const nlohmann::json& json, T& value,
                                                     std::source_location loc) {
    return _GetJsonValue<T>(json, value, nullptr, loc);
}

template<typename T>
typename JsonTemplateTypeCheck<T>::type GetJsonValue(const nlohmann::json& json,
                                                     std::string_view name_view, T& value,
                                                     bool warn, std::source_location loc) {
    std::string name { name_view };
    if (! json.contains(name)) {
        if (warn)
            rstd_info("read json \"{}\" not a key at {}({}:{})",
                      name,
                      std::string_view(loc.function_name()),
                      std::string_view(loc.file_name()),
                      loc.line());
        return false;
    } else if (json.at(name).is_null()) {
        if (warn)
            rstd_info("read json \"{}\" is null at {}({}:{})",
                      name,
                      std::string_view(loc.function_name()),
                      std::string_view(loc.file_name()),
                      loc.line());
        return false;
    }
    return _GetJsonValue<T>(json.at(name), value, name.c_str(), loc);
}

#define T_IMPL_GET_JSON(TYPE)                                      \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>( \
        const nlohmann::json&, TYPE&, std::source_location);       \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>( \
        const nlohmann::json&, std::string_view, TYPE&, bool, std::source_location);

T_IMPL_GET_JSON(bool);
T_IMPL_GET_JSON(int32_t);
T_IMPL_GET_JSON(uint32_t);
T_IMPL_GET_JSON(float);
T_IMPL_GET_JSON(double);
T_IMPL_GET_JSON(std::string);
T_IMPL_GET_JSON(std::vector<float>);
T_IMPL_GET_JSON(std::vector<int32_t>);

template<std::size_t N>
using iarray = std::array<int, N>;
T_IMPL_GET_JSON(iarray<3>);

template<std::size_t N>
using farray = std::array<float, N>;
T_IMPL_GET_JSON(farray<2>);
T_IMPL_GET_JSON(farray<3>);

} // namespace sr
