// util/toml_lite.cpp — see toml_lite.hpp.

#include "agentty/util/toml_lite.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace agentty::util::toml_lite {

using json = nlohmann::json;

namespace {

struct Parser {
    std::string_view s;
    std::size_t      i = 0;
    int              line = 1;
    std::string      err;

    [[nodiscard]] bool eof() const noexcept { return i >= s.size(); }
    [[nodiscard]] char peek(std::size_t k = 0) const noexcept {
        return i + k < s.size() ? s[i + k] : '\0';
    }
    bool fail(std::string what) {
        if (err.empty()) err = "line " + std::to_string(line) + ": " + std::move(what);
        return false;
    }

    void skip_ws() {
        while (!eof() && (peek() == ' ' || peek() == '\t')) ++i;
    }
    void skip_comment() {
        if (peek() == '#')
            while (!eof() && peek() != '\n') ++i;
    }
    // Whitespace, newlines and comments: used inside arrays.
    void skip_all() {
        for (;;) {
            skip_ws();
            skip_comment();
            if (peek() == '\r' && peek(1) == '\n') { i += 2; ++line; continue; }
            if (peek() == '\n') { ++i; ++line; continue; }
            return;
        }
    }
    // After a key/value or header: only a comment and the line end may follow.
    bool end_of_line() {
        skip_ws();
        skip_comment();
        if (eof()) return true;
        if (peek() == '\r' && peek(1) == '\n') { i += 2; ++line; return true; }
        if (peek() == '\n') { ++i; ++line; return true; }
        return fail("expected end of line");
    }

    static void put_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool escape(std::string& out) {
        // at the char after '\'
        const char c = peek();
        ++i;
        switch (c) {
            case 'b': out += '\b'; return true;
            case 't': out += '\t'; return true;
            case 'n': out += '\n'; return true;
            case 'f': out += '\f'; return true;
            case 'r': out += '\r'; return true;
            case 'e': out += '\x1b'; return true;
            case '"': out += '"'; return true;
            case '\\': out += '\\'; return true;
            case 'u': case 'U': {
                const int n = c == 'u' ? 4 : 8;
                std::uint32_t cp = 0;
                for (int k = 0; k < n; ++k) {
                    const char h = peek();
                    ++i;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= std::uint32_t(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= std::uint32_t(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= std::uint32_t(h - 'A' + 10);
                    else return fail("bad unicode escape");
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return fail("bad unicode escape");
                put_utf8(out, cp);
                return true;
            }
            default: return fail("bad escape");
        }
    }

    bool string(std::string& out) {
        const char q = peek();
        const bool multi = peek(1) == q && peek(2) == q;
        if (multi) {
            i += 3;
            // A newline right after the opening quotes is dropped.
            if (peek() == '\r' && peek(1) == '\n') { i += 2; ++line; }
            else if (peek() == '\n') { ++i; ++line; }
            for (;;) {
                if (eof()) return fail("unterminated string");
                if (peek() == q && peek(1) == q && peek(2) == q) {
                    i += 3;
                    // Up to two extra quotes may close the string.
                    for (int k = 0; k < 2 && peek() == q; ++k) { out += q; ++i; }
                    return true;
                }
                const char c = peek();
                if (q == '"' && c == '\\') {
                    ++i;
                    // Line-ending backslash: trim the newline and following space.
                    std::size_t j = i;
                    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                    if (j < s.size() && (s[j] == '\n' || s[j] == '\r')) {
                        i = j;
                        while (!eof() && (peek() == ' ' || peek() == '\t'
                                          || peek() == '\n' || peek() == '\r')) {
                            if (peek() == '\n') ++line;
                            ++i;
                        }
                        continue;
                    }
                    if (!escape(out)) return false;
                    continue;
                }
                if (c == '\n') ++line;
                out += c;
                ++i;
            }
        }
        ++i;
        for (;;) {
            if (eof() || peek() == '\n') return fail("unterminated string");
            const char c = peek();
            if (c == q) { ++i; return true; }
            if (q == '"' && c == '\\') {
                ++i;
                if (!escape(out)) return false;
                continue;
            }
            out += c;
            ++i;
        }
    }

    [[nodiscard]] static bool bare_char(char c) noexcept {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    bool simple_key(std::string& out) {
        skip_ws();
        if (peek() == '"' || peek() == '\'') {
            if (peek(1) == peek() && peek(2) == peek())
                return fail("multi-line string as key");
            return string(out);
        }
        const std::size_t start = i;
        while (!eof() && bare_char(peek())) ++i;
        if (i == start) return fail("expected a key");
        out.assign(s.substr(start, i - start));
        return true;
    }

    bool key(std::vector<std::string>& parts) {
        for (;;) {
            std::string k;
            if (!simple_key(k)) return false;
            parts.push_back(std::move(k));
            skip_ws();
            if (peek() != '.') return true;
            ++i;
        }
    }

    // Bare scalar token: numbers, booleans, dates.
    bool scalar(json& out) {
        const std::size_t start = i;
        while (!eof()) {
            const char c = peek();
            if (c == ',' || c == ']' || c == '}' || c == '#' || c == '\n' || c == '\r'
                || c == '\t')
                break;
            // A space can sit inside a date-time ("1979-05-27 07:32:00").
            if (c == ' ') {
                const bool date_gap = i > start && s[i - 1] >= '0' && s[i - 1] <= '9'
                                   && peek(1) >= '0' && peek(1) <= '9'
                                   && s.substr(start, i - start).find('-') != std::string_view::npos;
                if (!date_gap) break;
            }
            ++i;
        }
        std::string tok{s.substr(start, i - start)};
        if (tok.empty()) return fail("expected a value");
        if (tok == "true") { out = true; return true; }
        if (tok == "false") { out = false; return true; }
        if (tok == "inf" || tok == "+inf") { out = std::numeric_limits<double>::infinity(); return true; }
        if (tok == "-inf") { out = -std::numeric_limits<double>::infinity(); return true; }
        if (tok == "nan" || tok == "+nan" || tok == "-nan") { out = nullptr; return true; }

        std::string clean;
        clean.reserve(tok.size());
        for (char c : tok) if (c != '_') clean += c;
        try {
            if (clean.size() > 2 && clean[0] == '0'
                && (clean[1] == 'x' || clean[1] == 'o' || clean[1] == 'b')) {
                const int base = clean[1] == 'x' ? 16 : clean[1] == 'o' ? 8 : 2;
                std::size_t used = 0;
                const auto v = std::stoll(clean.substr(2), &used, base);
                if (used == clean.size() - 2) { out = v; return true; }
            }
            const bool looks_float = clean.find_first_of(".eE") != std::string::npos
                                  && clean.find(':') == std::string::npos;
            const bool looks_date = clean.find(':') != std::string::npos
                                 || (clean.size() >= 10 && clean[4] == '-' && clean[7] == '-');
            if (looks_date) { out = tok; return true; }
            std::size_t used = 0;
            if (looks_float) {
                const double d = std::stod(clean, &used);
                if (used == clean.size()) { out = d; return true; }
            } else {
                const long long v = std::stoll(clean, &used, 10);
                if (used == clean.size()) { out = v; return true; }
            }
        } catch (...) {
        }
        return fail("bad value '" + tok + "'");
    }

    bool value(json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        skip_ws();
        const char c = peek();
        if (c == '"' || c == '\'') {
            std::string str;
            if (!string(str)) return false;
            out = std::move(str);
            return true;
        }
        if (c == '[') {
            ++i;
            out = json::array();
            for (;;) {
                skip_all();
                if (peek() == ']') { ++i; return true; }
                json v;
                if (!value(v, depth + 1)) return false;
                out.push_back(std::move(v));
                skip_all();
                if (peek() == ',') { ++i; continue; }
                if (peek() == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '{') {
            ++i;
            out = json::object();
            skip_all();
            if (peek() == '}') { ++i; return true; }
            for (;;) {
                std::vector<std::string> parts;
                if (!key(parts)) return false;
                skip_ws();
                if (peek() != '=') return fail("expected '='");
                ++i;
                json v;
                if (!value(v, depth + 1)) return false;
                if (!assign(out, parts, std::move(v))) return false;
                skip_all();
                if (peek() == ',') { ++i; skip_all(); continue; }
                if (peek() == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        return scalar(out);
    }

    // Walk `parts` from `at`, creating tables. An array of tables resolves
    // to its last element.
    json* walk(json* at, const std::vector<std::string>& parts, std::size_t n) {
        for (std::size_t k = 0; k < n; ++k) {
            json& next = (*at)[parts[k]];
            if (next.is_null()) next = json::object();
            if (next.is_array() && !next.empty() && next.back().is_object()) {
                at = &next.back();
                continue;
            }
            if (!next.is_object()) {
                fail("'" + parts[k] + "' is not a table");
                return nullptr;
            }
            at = &next;
        }
        return at;
    }

    bool assign(json& table, const std::vector<std::string>& parts, json v) {
        json* at = walk(&table, parts, parts.size() - 1);
        if (!at) return false;
        if (at->contains(parts.back()))
            return fail("duplicate key '" + parts.back() + "'");
        (*at)[parts.back()] = std::move(v);
        return true;
    }

    bool document(json& root) {
        root = json::object();
        json* current = &root;
        for (;;) {
            skip_all();
            if (eof()) return true;
            if (peek() == '[') {
                const bool array = peek(1) == '[';
                i += array ? 2 : 1;
                std::vector<std::string> parts;
                if (!key(parts)) return false;
                skip_ws();
                if (array) {
                    if (peek() != ']' || peek(1) != ']') return fail("expected ']]'");
                    i += 2;
                    json* parent = walk(&root, parts, parts.size() - 1);
                    if (!parent) return false;
                    json& arr = (*parent)[parts.back()];
                    if (arr.is_null()) arr = json::array();
                    if (!arr.is_array()) return fail("'" + parts.back() + "' is not an array");
                    arr.push_back(json::object());
                    current = &arr.back();
                } else {
                    if (peek() != ']') return fail("expected ']'");
                    ++i;
                    current = walk(&root, parts, parts.size());
                    if (!current) return false;
                }
                if (!end_of_line()) return false;
                continue;
            }
            std::vector<std::string> parts;
            if (!key(parts)) return false;
            skip_ws();
            if (peek() != '=') return fail("expected '='");
            ++i;
            json v;
            if (!value(v, 0)) return false;
            if (!assign(*current, parts, std::move(v))) return false;
            if (!end_of_line()) return false;
        }
    }
};

}  // namespace

bool parse(std::string_view text, json& out, std::string& error) {
    Parser p{text};
    // A UTF-8 BOM is not content.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") p.i = 3;
    if (p.document(out)) return true;
    error = p.err.empty() ? std::string{"parse error"} : p.err;
    out = json::object();
    return false;
}

}  // namespace agentty::util::toml_lite
