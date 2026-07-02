module;

export module sr.pkg.parse:shader_lex;
import rstd.cppstd;

export namespace sr::shader_lex
{

inline bool IsHSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsVSpace(char c) { return c == '\n' || c == '\r'; }
inline bool IsIdStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool IsIdCont(char c) { return IsIdStart(c) || (c >= '0' && c <= '9'); }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

inline bool IsPrecisionQualifier(std::string_view ident) noexcept {
    return ident == "lowp" || ident == "mediump" || ident == "highp";
}

struct TypeName {
    std::string_view type;
    std::string_view name;
};

// Hand-rolled scanner over a string_view. Pos always points at the next byte
// to consume; Skip*/Match*/Read* primitives advance on success and stay put
// on failure so the caller can probe alternatives without explicit Save.
class Cursor {
public:
    explicit Cursor(std::string_view src) noexcept: m_src(src), m_pos(0) {}
    Cursor(std::string_view src, std::size_t pos) noexcept: m_src(src), m_pos(pos) {}

    std::size_t      Pos() const noexcept { return m_pos; }
    std::string_view Source() const noexcept { return m_src; }
    bool             Eof() const noexcept { return m_pos >= m_src.size(); }
    char             Peek(std::size_t off = 0) const noexcept {
        return (m_pos + off < m_src.size()) ? m_src[m_pos + off] : '\0';
    }
    void SeekTo(std::size_t pos) noexcept { m_pos = pos > m_src.size() ? m_src.size() : pos; }
    void Advance(std::size_t n = 1) noexcept { SeekTo(m_pos + n); }

    bool        AtLineStart() const noexcept { return m_pos == 0 || m_src[m_pos - 1] == '\n'; }
    std::size_t LineStart() const noexcept {
        if (m_pos == 0) return 0;
        auto nl = m_src.rfind('\n', m_pos - 1);
        return nl == std::string_view::npos ? 0 : nl + 1;
    }
    std::size_t LineEnd() const noexcept {
        auto nl = m_src.find('\n', m_pos);
        return nl == std::string_view::npos ? m_src.size() : nl;
    }
    std::string_view CurrentLine() const noexcept {
        std::size_t s = LineStart();
        std::size_t e = LineEnd();
        return m_src.substr(s, e - s);
    }
    void SkipLine() noexcept {
        std::size_t e = LineEnd();
        SeekTo(e < m_src.size() ? e + 1 : e);
    }

    void SkipHSpace() noexcept {
        while (m_pos < m_src.size() && IsHSpace(m_src[m_pos])) ++m_pos;
    }
    void SkipToEol() noexcept {
        while (m_pos < m_src.size() && m_src[m_pos] != '\n') ++m_pos;
    }
    void SkipAllTrivia() noexcept {
        while (m_pos < m_src.size()) {
            char c = m_src[m_pos];
            if (IsHSpace(c) || IsVSpace(c)) {
                ++m_pos;
                continue;
            }
            if (c == '/' && m_pos + 1 < m_src.size()) {
                if (m_src[m_pos + 1] == '/') {
                    SkipToEol();
                    continue;
                }
                if (m_src[m_pos + 1] == '*') {
                    m_pos += 2;
                    while (m_pos + 1 < m_src.size() &&
                           ! (m_src[m_pos] == '*' && m_src[m_pos + 1] == '/')) {
                        ++m_pos;
                    }
                    if (m_pos + 1 < m_src.size())
                        m_pos += 2;
                    else
                        m_pos = m_src.size();
                    continue;
                }
            }
            break;
        }
    }

    std::optional<std::string_view> ReadIdent() noexcept {
        if (m_pos >= m_src.size() || ! IsIdStart(m_src[m_pos])) return std::nullopt;
        std::size_t s = m_pos++;
        while (m_pos < m_src.size() && IsIdCont(m_src[m_pos])) ++m_pos;
        return m_src.substr(s, m_pos - s);
    }
    std::optional<std::string_view> ReadInt() noexcept {
        if (m_pos >= m_src.size() || ! IsDigit(m_src[m_pos])) return std::nullopt;
        std::size_t s = m_pos++;
        while (m_pos < m_src.size() && IsDigit(m_src[m_pos])) ++m_pos;
        return m_src.substr(s, m_pos - s);
    }
    // Match `[...]`. Returns the bracketed text including brackets. Content
    // between [] isn't validated as integer — the caller decides.
    std::optional<std::string_view> ReadArraySuffix() noexcept {
        if (m_pos >= m_src.size() || m_src[m_pos] != '[') return std::nullopt;
        std::size_t s = m_pos;
        std::size_t p = m_pos + 1;
        while (p < m_src.size() && m_src[p] != ']' && m_src[p] != '\n') ++p;
        if (p >= m_src.size() || m_src[p] != ']') return std::nullopt;
        ++p;
        m_pos = p;
        return m_src.substr(s, p - s);
    }

    bool MatchChar(char c) noexcept {
        if (m_pos < m_src.size() && m_src[m_pos] == c) {
            ++m_pos;
            return true;
        }
        return false;
    }
    bool MatchPunct(std::string_view s) noexcept {
        if (m_pos + s.size() > m_src.size()) return false;
        if (m_src.substr(m_pos, s.size()) != s) return false;
        m_pos += s.size();
        return true;
    }
    // ident-边界感知 keyword 匹配。"uniform" 不会匹配 "uniformly".
    bool MatchKeyword(std::string_view kw) noexcept {
        if (m_pos + kw.size() > m_src.size()) return false;
        if (m_src.substr(m_pos, kw.size()) != kw) return false;
        if (m_pos + kw.size() < m_src.size() && IsIdCont(m_src[m_pos + kw.size()])) return false;
        m_pos += kw.size();
        return true;
    }
    // `#` (允许前置 H-space) + name + ident 边界. 成功后光标在 name 之后。
    bool MatchHashDirective(std::string_view name) noexcept {
        std::size_t save = m_pos;
        SkipHSpace();
        if (m_pos >= m_src.size() || m_src[m_pos] != '#') {
            m_pos = save;
            return false;
        }
        ++m_pos;
        SkipHSpace();
        if (! MatchKeyword(name)) {
            m_pos = save;
            return false;
        }
        return true;
    }

    struct Saved {
        std::size_t pos;
    };
    Saved Save() const noexcept { return { m_pos }; }
    void  Restore(Saved s) noexcept { m_pos = s.pos; }

private:
    std::string_view m_src;
    std::size_t      m_pos;
};

inline std::optional<TypeName> ReadTypeName(Cursor& c) noexcept {
    auto type = c.ReadIdent();
    if (! type) return std::nullopt;
    c.SkipHSpace();
    if (IsPrecisionQualifier(*type)) {
        type = c.ReadIdent();
        if (! type) return std::nullopt;
        c.SkipHSpace();
    }
    auto name = c.ReadIdent();
    if (! name) return std::nullopt;
    return TypeName { *type, *name };
}

// Walks one source line at a time. Tracks block-comment state so that a
// `/* ... \n ... */` spanning multiple physical lines turns every line it
// covers into the empty view — the WE annotation collector relies on this
// so `uniform vec4 X;` inside a block comment never gets emitted.
class LineWalker {
public:
    explicit LineWalker(std::string_view src) noexcept
        : m_src(src), m_pos(0), m_line_start(0), m_line_end(0), m_in_block(false) {
        Recompute();
    }
    bool             Done() const noexcept { return m_pos > m_src.size(); }
    std::size_t      LineStart() const noexcept { return m_line_start; }
    std::size_t      LineEnd() const noexcept { return m_line_end; }
    std::string_view Line() const noexcept {
        // When the entire line is masked by an enclosing block comment, hide
        // it from callers so token scans never see the masked text.
        if (m_line_masked) return std::string_view {};
        return m_src.substr(m_line_start, m_line_end - m_line_start);
    }
    // Raw line text including any masked content. Stripper passes use this
    // so the original bytes (including block-comment text) survive into the
    // output unchanged.
    std::string_view RawLine() const noexcept {
        return m_src.substr(m_line_start, m_line_end - m_line_start);
    }
    Cursor LineCursor() const noexcept {
        // The returned Cursor scans only the visible part of the line.
        return Cursor { Line() };
    }
    void Step() noexcept {
        m_pos = (m_line_end < m_src.size()) ? m_line_end + 1 : m_src.size() + 1;
        Recompute();
    }

private:
    void Recompute() noexcept {
        if (m_pos > m_src.size()) {
            m_line_start = m_line_end = m_src.size();
            m_line_masked             = false;
            return;
        }
        m_line_start = m_pos;
        auto nl      = m_src.find('\n', m_pos);
        m_line_end   = (nl == std::string_view::npos) ? m_src.size() : nl;

        // Walk the line characterwise to update block-comment state.
        m_line_masked               = m_in_block;
        std::size_t p               = m_line_start;
        bool        starts_in_block = m_in_block;
        while (p < m_line_end) {
            if (m_in_block) {
                if (p + 1 < m_line_end && m_src[p] == '*' && m_src[p + 1] == '/') {
                    m_in_block = false;
                    p += 2;
                    // If a `*/` closes mid-line, content after it is visible —
                    // unmask the line.
                    if (starts_in_block) m_line_masked = false;
                } else {
                    ++p;
                }
            } else {
                if (p + 1 < m_line_end && m_src[p] == '/' && m_src[p + 1] == '/') {
                    // Line comment terminates the line for block-state purposes.
                    p = m_line_end;
                    break;
                }
                if (p + 1 < m_line_end && m_src[p] == '/' && m_src[p + 1] == '*') {
                    m_in_block = true;
                    p += 2;
                } else {
                    ++p;
                }
            }
        }
        // Note: if `m_in_block` is true on EOL the rest of the line is in a
        // block comment. We still expose the visible prefix via Line() — only
        // lines that started inside a block comment are masked.
    }

    std::string_view m_src;
    std::size_t      m_pos;
    std::size_t      m_line_start;
    std::size_t      m_line_end;
    bool             m_in_block;
    bool             m_line_masked { false };
};

// Token stream layered over Cursor. Tokens preserve the underlying bytes
// (text + offset) so callers can splice / re-emit the source unchanged. The
// lexer is line-aware: Newline / HSpace / LineComment / BlockComment are
// each their own kind so directive parsing stays straightforward.
enum class TokenKind : std::uint8_t
{
    Eof,
    Newline,      // single '\n'
    HSpace,       // run of ' ' / '\t'
    LineComment,  // includes the leading '//' and runs up to (not including) '\n'
    BlockComment, // includes leading '/*' and trailing '*/'
    Ident,        // [A-Za-z_][A-Za-z0-9_]*
    Int,          // [0-9]+
    String,       // "..." including quotes; consumed up to '\n' / EOF if unterminated
    Hash,         // single '#'
    Punct,        // any single-byte ASCII punctuation not covered above
    Unknown,      // any other byte (non-ASCII, control chars)
};

struct Token {
    TokenKind        kind;
    std::string_view text;
    std::size_t      offset;
};

class Lexer {
public:
    explicit Lexer(std::string_view src) noexcept: m_src(src), m_pos(0) {}

    std::string_view Source() const noexcept { return m_src; }
    std::size_t      Pos() const noexcept { return m_pos; }
    void             SeekTo(std::size_t p) noexcept { m_pos = p > m_src.size() ? m_src.size() : p; }
    bool             Eof() const noexcept { return m_pos >= m_src.size(); }

    Token Peek() const noexcept {
        std::size_t save                = m_pos;
        Token       t                   = const_cast<Lexer*>(this)->ScanOne();
        const_cast<Lexer*>(this)->m_pos = save;
        return t;
    }
    Token Next() noexcept { return ScanOne(); }

    // Convenience: skip tokens whose kind matches the predicate, then return
    // the next "interesting" token. Used by directive parsing to skip HSpace.
    template<typename Pred>
    Token NextSkip(Pred pred) noexcept {
        for (;;) {
            Token t = ScanOne();
            if (! pred(t.kind)) return t;
            if (t.kind == TokenKind::Eof) return t;
        }
    }

    struct Saved {
        std::size_t pos;
    };
    Saved Save() const noexcept { return { m_pos }; }
    void  Restore(Saved s) noexcept { m_pos = s.pos; }

private:
    Token ScanOne() noexcept {
        if (m_pos >= m_src.size()) return { TokenKind::Eof, {}, m_src.size() };
        std::size_t start = m_pos;
        char        c0    = m_src[m_pos];

        if (c0 == '\n') {
            ++m_pos;
            return { TokenKind::Newline, m_src.substr(start, 1), start };
        }
        if (IsHSpace(c0)) {
            while (m_pos < m_src.size() && IsHSpace(m_src[m_pos])) ++m_pos;
            return { TokenKind::HSpace, m_src.substr(start, m_pos - start), start };
        }
        if (c0 == '/' && m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '/') {
            m_pos += 2;
            while (m_pos < m_src.size() && m_src[m_pos] != '\n') ++m_pos;
            return { TokenKind::LineComment, m_src.substr(start, m_pos - start), start };
        }
        if (c0 == '/' && m_pos + 1 < m_src.size() && m_src[m_pos + 1] == '*') {
            m_pos += 2;
            while (m_pos + 1 < m_src.size() && ! (m_src[m_pos] == '*' && m_src[m_pos + 1] == '/'))
                ++m_pos;
            if (m_pos + 1 < m_src.size())
                m_pos += 2;
            else
                m_pos = m_src.size();
            return { TokenKind::BlockComment, m_src.substr(start, m_pos - start), start };
        }
        if (IsIdStart(c0)) {
            ++m_pos;
            while (m_pos < m_src.size() && IsIdCont(m_src[m_pos])) ++m_pos;
            return { TokenKind::Ident, m_src.substr(start, m_pos - start), start };
        }
        if (IsDigit(c0)) {
            ++m_pos;
            while (m_pos < m_src.size() && IsDigit(m_src[m_pos])) ++m_pos;
            return { TokenKind::Int, m_src.substr(start, m_pos - start), start };
        }
        if (c0 == '"') {
            ++m_pos;
            while (m_pos < m_src.size() && m_src[m_pos] != '"' && m_src[m_pos] != '\n') ++m_pos;
            if (m_pos < m_src.size() && m_src[m_pos] == '"') ++m_pos;
            return { TokenKind::String, m_src.substr(start, m_pos - start), start };
        }
        if (c0 == '#') {
            ++m_pos;
            return { TokenKind::Hash, m_src.substr(start, 1), start };
        }
        if ((unsigned char)c0 >= 0x20 && (unsigned char)c0 < 0x7f) {
            ++m_pos;
            return { TokenKind::Punct, m_src.substr(start, 1), start };
        }
        ++m_pos;
        return { TokenKind::Unknown, m_src.substr(start, 1), start };
    }

    std::string_view m_src;
    std::size_t      m_pos;
};

enum class PpKind
{
    None,
    If,
    Ifdef,
    Ifndef,
    Elif,
    Else,
    Endif,
    Define,
    Undef,
    Include,
    Require,
    Pragma,
    Extension,
    Version,
    Other,
};

inline PpKind ClassifyPreproc(Cursor c) noexcept {
    Lexer lx(c.Source().substr(c.Pos()));
    // First non-HSpace token must be Hash.
    Token t = lx.NextSkip([](TokenKind k) {
        return k == TokenKind::HSpace;
    });
    if (t.kind != TokenKind::Hash) return PpKind::None;
    // Directive name after optional HSpace.
    t = lx.NextSkip([](TokenKind k) {
        return k == TokenKind::HSpace;
    });
    if (t.kind != TokenKind::Ident) return PpKind::Other;
    auto id = t.text;
    if (id == "if") return PpKind::If;
    if (id == "ifdef") return PpKind::Ifdef;
    if (id == "ifndef") return PpKind::Ifndef;
    if (id == "elif") return PpKind::Elif;
    if (id == "else") return PpKind::Else;
    if (id == "endif") return PpKind::Endif;
    if (id == "define") return PpKind::Define;
    if (id == "undef") return PpKind::Undef;
    if (id == "include") return PpKind::Include;
    if (id == "require") return PpKind::Require;
    if (id == "pragma") return PpKind::Pragma;
    if (id == "extension") return PpKind::Extension;
    if (id == "version") return PpKind::Version;
    return PpKind::Other;
}

} // namespace sr::shader_lex
