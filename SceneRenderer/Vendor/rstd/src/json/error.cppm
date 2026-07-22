module;
#include <rstd/macro.hpp>

export module rstd.json:error;
export import rstd.core;
import rstd.alloc;

namespace rstd::json::detail
{

enum class ErrorCode : u8
{
    EofWhileParsingList,
    EofWhileParsingObject,
    EofWhileParsingComment,
    EofWhileParsingString,
    EofWhileParsingValue,
    ExpectedColon,
    ExpectedListCommaOrEnd,
    ExpectedObjectCommaOrEnd,
    ExpectedSomeIdent,
    ExpectedSomeValue,
    InvalidEscape,
    InvalidNumber,
    NumberOutOfRange,
    InvalidUnicodeCodePoint,
    ControlCharacterWhileParsingString,
    KeyMustBeAString,
    LoneLeadingSurrogateInHexEscape,
    TrailingComma,
    TrailingCharacters,
    UnexpectedEndOfHexEscape,
    RecursionLimitExceeded,
};

class Parser;

} // namespace rstd::json::detail

export namespace rstd::json
{

enum class Category : u8
{
    Syntax,
    Eof,
};

class Error {
public:
    detail::ErrorCode code_;
    usize             line_;
    usize             column_;

    constexpr Error(detail::ErrorCode code, usize line, usize column) noexcept
        : code_(code), line_(line), column_(column) {}

    friend class detail::Parser;
#if !defined(RSTD_OS_WINDOWS)
    template<typename, typename>
    friend struct rstd::Impl;
#endif
    [[nodiscard]]
    constexpr auto line() const noexcept -> usize {
        return line_;
    }
    [[nodiscard]]
    constexpr auto column() const noexcept -> usize {
        return column_;
    }

    [[nodiscard]]
    constexpr auto classify() const noexcept -> Category {
        switch (code_) {
        case detail::ErrorCode::EofWhileParsingList:
        case detail::ErrorCode::EofWhileParsingObject:
        case detail::ErrorCode::EofWhileParsingComment:
        case detail::ErrorCode::EofWhileParsingString:
        case detail::ErrorCode::EofWhileParsingValue: return Category::Eof;
        default: return Category::Syntax;
        }
    }

    [[nodiscard]]
    constexpr auto is_syntax() const noexcept -> bool {
        return classify() == Category::Syntax;
    }
    [[nodiscard]]
    constexpr auto is_eof() const noexcept -> bool {
        return classify() == Category::Eof;
    }
};

} // namespace rstd::json

namespace rstd
{

template<>
struct Impl<fmt::Display, json::Error> : ImplBase<json::Error> {
    auto fmt(fmt::Formatter& formatter) const -> bool;
};

template<>
struct Impl<fmt::Debug, json::Error> : ImplBase<json::Error> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};

} // namespace rstd

namespace rstd
{

auto Impl<fmt::Display, json::Error>::fmt(fmt::Formatter& formatter) const -> bool {
    const char* message = "JSON syntax error";
    switch (this->self().code_) {
    case json::detail::ErrorCode::EofWhileParsingList: message = "EOF while parsing a list"; break;
    case json::detail::ErrorCode::EofWhileParsingObject:
        message = "EOF while parsing an object";
        break;
    case json::detail::ErrorCode::EofWhileParsingComment:
        message = "EOF while parsing a comment";
        break;
    case json::detail::ErrorCode::EofWhileParsingString:
        message = "EOF while parsing a string";
        break;
    case json::detail::ErrorCode::EofWhileParsingValue:
        message = "EOF while parsing a value";
        break;
    case json::detail::ErrorCode::ExpectedColon: message = "expected `:`"; break;
    case json::detail::ErrorCode::ExpectedListCommaOrEnd: message = "expected `,` or `]`"; break;
    case json::detail::ErrorCode::ExpectedObjectCommaOrEnd: message = "expected `,` or `}`"; break;
    case json::detail::ErrorCode::ExpectedSomeIdent: message = "expected ident"; break;
    case json::detail::ErrorCode::ExpectedSomeValue: message = "expected value"; break;
    case json::detail::ErrorCode::InvalidEscape: message = "invalid escape"; break;
    case json::detail::ErrorCode::InvalidNumber: message = "invalid number"; break;
    case json::detail::ErrorCode::NumberOutOfRange: message = "number out of range"; break;
    case json::detail::ErrorCode::InvalidUnicodeCodePoint:
        message = "invalid unicode code point";
        break;
    case json::detail::ErrorCode::ControlCharacterWhileParsingString:
        message = "control character (\\u0000-\\u001F) found while parsing a string";
        break;
    case json::detail::ErrorCode::KeyMustBeAString: message = "key must be a string"; break;
    case json::detail::ErrorCode::LoneLeadingSurrogateInHexEscape:
        message = "lone leading surrogate in hex escape";
        break;
    case json::detail::ErrorCode::TrailingComma: message = "trailing comma"; break;
    case json::detail::ErrorCode::TrailingCharacters: message = "trailing characters"; break;
    case json::detail::ErrorCode::UnexpectedEndOfHexEscape:
        message = "unexpected end of hex escape";
        break;
    case json::detail::ErrorCode::RecursionLimitExceeded:
        message = "recursion limit exceeded";
        break;
    }
    return formatter.write_fmt(fmt::Arguments::make(
        "{} at line {} column {}", message, this->self().line_, this->self().column_));
}

} // namespace rstd
