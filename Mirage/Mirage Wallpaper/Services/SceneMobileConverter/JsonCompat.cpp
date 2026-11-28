//
//  Mirage Wallpaper
//
//  Copyright © 2026 王孝慈. All rights reserved.
//

#include "JsonCompat.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace mirage::mobile {

namespace {

std::string SanitizeJsonCpp(std::string_view src) {
    // Pass 1: strip `//` and `/* ... */` comments outside quoted strings.
    std::string no_comments;
    no_comments.reserve(src.size());
    size_t i = 0;
    bool in_str = false;
    while (i < src.size()) {
        char c = src[i];
        if (in_str) {
            no_comments.push_back(c);
            if (c == '\\' && i + 1 < src.size()) {
                no_comments.push_back(src[i + 1]);
                i += 2;
                continue;
            }
            if (c == '"') {
                in_str = false;
            }
            ++i;
        } else if (c == '"') {
            in_str = true;
            no_comments.push_back(c);
            ++i;
        } else if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            i += 2;
            while (i < src.size() && src[i] != '\n') ++i;
        } else if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i = std::min(i + 2, src.size());
        } else {
            no_comments.push_back(c);
            ++i;
        }
    }

    // Pass 2: strip trailing commas before `}` or `]` outside quoted strings.
    std::string out;
    out.reserve(no_comments.size());
    size_t j = 0;
    in_str = false;
    while (j < no_comments.size()) {
        char c = no_comments[j];
        if (in_str) {
            out.push_back(c);
            if (c == '\\' && j + 1 < no_comments.size()) {
                out.push_back(no_comments[j + 1]);
                j += 2;
                continue;
            }
            if (c == '"') {
                in_str = false;
            }
            ++j;
        } else if (c == '"') {
            in_str = true;
            out.push_back(c);
            ++j;
        } else if (c == ',') {
            size_t k = j + 1;
            while (k < no_comments.size() &&
                   (no_comments[k] == ' ' || no_comments[k] == '\t' ||
                    no_comments[k] == '\r' || no_comments[k] == '\n')) {
                ++k;
            }
            if (k < no_comments.size() && (no_comments[k] == '}' || no_comments[k] == ']')) {
                ++j;
                continue;
            }
            out.push_back(',');
            ++j;
        } else {
            out.push_back(c);
            ++j;
        }
    }
    return out;
}

void WriteTabs(std::string& out, size_t indent) {
    out.append(indent, '\t');
}

void WriteJsonEscapedStr(std::string& out, std::string_view s) {
    out.push_back('"');
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        const char* esc = nullptr;
        switch (b) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case 0x08: esc = "\\b"; break;
            case 0x0c: esc = "\\f"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default:
                if (b >= 0x20) continue;
                break;
        }
        if (start < i) {
            out.append(s.data() + start, i - start);
        }
        if (esc) {
            out.append(esc);
        } else {
            static constexpr char kHex[] = "0123456789abcdef";
            out.append("\\u00");
            out.push_back(kHex[(b >> 4) & 0x0fU]);
            out.push_back(kHex[b & 0x0fU]);
        }
        start = i + 1;
    }
    if (start < s.size()) {
        out.append(s.data() + start, s.size() - start);
    }
    out.push_back('"');
}

std::string FormatJsonCppF64(double f) {
    if (!std::isfinite(f)) return "null";
    if (f == 0.0) {
        return std::signbit(f) ? "-0.0" : "0.0";
    }
    const char* sign = std::signbit(f) ? "-" : "";
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::scientific << std::setprecision(7) << std::fabs(f);
    std::string sci = oss.str();

    size_t e_pos = sci.find('e');
    if (e_pos == std::string::npos) return sci;
    std::string mant = sci.substr(0, e_pos);
    int exp = std::stoi(sci.substr(e_pos + 1));

    std::string digits;
    digits.reserve(mant.size());
    for (char c : mant) {
        if (c != '.') digits.push_back(c);
    }

    auto trim_trailing_zeros = [](std::string_view sv) {
        while (!sv.empty() && sv.back() == '0') sv.remove_suffix(1);
        return sv;
    };

    if (exp >= -4 && exp < 0) {
        size_t zeros = static_cast<size_t>(-exp - 1);
        std::string_view trimmed = trim_trailing_zeros(digits);
        std::string res = sign;
        res.append("0.");
        res.append(zeros, '0');
        res.append(trimmed);
        return res;
    }
    if (exp >= 0 && exp < 8) {
        size_t int_len = static_cast<size_t>(exp + 1);
        std::string_view int_part = std::string_view(digits).substr(0, int_len);
        std::string_view frac_part = trim_trailing_zeros(std::string_view(digits).substr(int_len));
        std::string res = sign;
        res.append(int_part);
        res.push_back('.');
        if (frac_part.empty()) {
            res.push_back('0');
        } else {
            res.append(frac_part);
        }
        return res;
    }

    std::string_view frac = trim_trailing_zeros(std::string_view(digits).substr(1));
    char exp_sign = exp >= 0 ? '+' : '-';
    int exp_abs = exp >= 0 ? exp : -exp;
    std::string res = sign;
    res.push_back(digits[0]);
    if (!frac.empty()) {
        res.push_back('.');
        res.append(frac);
    }
    res.push_back('e');
    res.push_back(exp_sign);
    if (exp_abs < 10) res.push_back('0');
    res.append(std::to_string(exp_abs));
    return res;
}

void WriteJsonCppValue(std::string& out, const Json& value, size_t indent);

std::optional<std::string> TryInlineJsonCppArray(const Json& arr) {
    if (arr.empty()) return std::string("[]");
    for (const auto& item : arr) {
        if (item.is_object() || item.is_array()) return std::nullopt;
    }
    std::string inline_str = "[ ";
    for (size_t idx = 0; idx < arr.size(); ++idx) {
        if (idx > 0) inline_str.append(", ");
        WriteJsonCppValue(inline_str, arr[idx], 0);
        if (inline_str.size() > 74) return std::nullopt;
    }
    inline_str.append(" ]");
    if (inline_str.size() <= 74) return inline_str;
    return std::nullopt;
}

void WriteJsonCppMultilineArray(std::string& out, const Json& arr, size_t indent) {
    out.append("[\r\n");
    size_t child_indent = indent + 1;
    for (size_t idx = 0; idx < arr.size(); ++idx) {
        if (idx > 0) out.append(",\r\n");
        WriteTabs(out, child_indent);
        WriteJsonCppValue(out, arr[idx], child_indent);
    }
    out.append("\r\n");
    WriteTabs(out, indent);
    out.push_back(']');
}

void WriteJsonCppValue(std::string& out, const Json& value, size_t indent) {
    if (value.is_null()) {
        out.append("null");
    } else if (value.is_boolean()) {
        out.append(value.get<bool>() ? "true" : "false");
    } else if (value.is_number_integer()) {
        out.append(std::to_string(value.get<int64_t>()));
    } else if (value.is_number_unsigned()) {
        out.append(std::to_string(value.get<uint64_t>()));
    } else if (value.is_number_float()) {
        out.append(FormatJsonCppF64(value.get<double>()));
    } else if (value.is_string()) {
        WriteJsonEscapedStr(out, value.get_ref<const std::string&>());
    } else if (value.is_array()) {
        if (auto inline_str = TryInlineJsonCppArray(value)) {
            out.append(*inline_str);
        } else {
            WriteJsonCppMultilineArray(out, value, indent);
        }
    } else if (value.is_object()) {
        if (value.empty()) {
            out.append("{}");
            return;
        }
        out.append("{\r\n");
        size_t child_indent = indent + 1;
        size_t idx = 0;
        for (const auto& [k, v] : value.items()) {
            if (idx++ > 0) out.append(",\r\n");
            WriteTabs(out, child_indent);
            WriteJsonEscapedStr(out, k);
            if (v.is_object() && !v.empty()) {
                out.append(" : \r\n");
                WriteTabs(out, child_indent);
                WriteJsonCppValue(out, v, child_indent);
            } else if (v.is_array()) {
                if (auto inline_str = TryInlineJsonCppArray(v)) {
                    out.append(" : ");
                    out.append(*inline_str);
                } else {
                    out.append(" : \r\n");
                    WriteTabs(out, child_indent);
                    WriteJsonCppMultilineArray(out, v, child_indent);
                }
            } else {
                out.append(" : ");
                WriteJsonCppValue(out, v, child_indent);
            }
        }
        out.append("\r\n");
        WriteTabs(out, indent);
        out.push_back('}');
    }
}

} // namespace

std::optional<Json> ParseJsonLenient(const uint8_t* data, size_t size) {
    if (!data || size == 0) return std::nullopt;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        size -= 3;
    }
    const char* begin = reinterpret_cast<const char*>(data);
    const char* end = begin + size;
    Json parsed = Json::parse(begin, end, nullptr, false, false);
    if (!parsed.is_discarded()) {
        return parsed;
    }
    std::string cleaned = SanitizeJsonCpp(std::string_view(begin, size));
    Json fallback = Json::parse(cleaned, nullptr, false, false);
    if (!fallback.is_discarded()) {
        return fallback;
    }
    return std::nullopt;
}

std::optional<Json> ParseJsonLenient(const std::vector<uint8_t>& bytes) {
    return ParseJsonLenient(bytes.data(), bytes.size());
}

std::optional<Json> ParseJsonLenient(std::string_view text) {
    return ParseJsonLenient(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string ToJsonCppStyledString(const Json& value) {
    std::string out;
    out.reserve(256);
    WriteJsonCppValue(out, value, 0);
    return out;
}

std::vector<uint8_t> ToJsonCppStyledBytes(const Json& value) {
    std::string s = ToJsonCppStyledString(value);
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace mirage::mobile
