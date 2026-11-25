#include "agentty/tool/util/call_leak.hpp"

#include <array>
#include <cctype>

namespace agentty::tools::util {

using nlohmann::json;

namespace {

// Fields that name a location or an identifier. A leaked wrapper lands in
// whichever of these the host's decoder was writing; free-text fields are
// deliberately absent (see the header).
constexpr std::array<std::string_view, 21> kLocationKeys = {
    "path", "file_path", "filepath", "filename", "file", "pathname",
    "dir", "directory", "target", "source", "destination", "cd", "cwd",
    "symbol", "ref", "url", "name", "branch", "start_point", "upstream",
    "old_path",
};
constexpr std::array<std::string_view, 4> kLocationListKeys = {
    "paths", "files", "file_paths", "commits",
};

bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool contains(std::string_view s, std::string_view needle) noexcept {
    return s.find(needle) != std::string_view::npos;
}

std::string_view strip_namespace(std::string_view n) noexcept {
    for (std::string_view p : {"functions.", "function.", "tools."})
        if (n.size() > p.size() && ieq(n.substr(0, p.size()), p))
            return n.substr(p.size());
    return n;
}

std::size_t skip_ws(std::string_view s, std::size_t i) noexcept {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
    return i;
}

// JSON structure that no real path or identifier carries: `}}` running into
// the next element (`}},{`, `}}]`), or a closing quote followed by an object
// or array end, or by `,"` (the next key).
bool has_structural_tail(std::string_view s) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '}') {
            std::size_t j = skip_ws(s, i + 1);
            if (j < s.size() && s[j] == '}') {
                j = skip_ws(s, j + 1);
                if (j < s.size() && (s[j] == ',' || s[j] == ']' || s[j] == '{'))
                    return true;
            }
        } else if (s[i] == '"') {
            std::size_t j = skip_ws(s, i + 1);
            if (j < s.size() && (s[j] == '}' || s[j] == ']')) return true;
            if (j < s.size() && s[j] == ',') {
                j = skip_ws(s, j + 1);
                if (j < s.size() && s[j] == '"') return true;
            }
        }
    }
    return false;
}

// Wrapper vocabulary written as JSON text inside a value (quoted, so a
// symbol that is merely NAMED recipient_name is not evidence).
bool has_wrapper_text(std::string_view s) noexcept {
    return contains(s, "\"recipient_name\"") || contains(s, "multi_tool_use.")
        || contains(s, "\"tool_uses\"") || contains(s, "\"parameters\"");
}

// A value that IS a wrapper token. Weak alone (a codebase may well have a
// symbol called `parameters`); it only joins a report another signal opened.
bool is_wrapper_token(std::string_view s) noexcept {
    return ieq(s, "recipient_name") || ieq(s, "parameters") || ieq(s, "tool_uses")
        || ieq(s, "multi_tool_use.parallel")
        || (s.size() > 10 && ieq(s.substr(0, 10), "functions."));
}

std::string clip(std::string_view s, std::size_t n = 60) {
    if (s.size() <= n) return std::string{s};
    return std::string{s.substr(0, n)} + "\xe2\x80\xa6";
}

// `read(path=a.go, symbol=f)`: a wrapped call, for the error text.
std::string describe(const json& item) {
    std::string name = "?";
    if (auto it = item.find("recipient_name"); it != item.end() && it->is_string())
        name = std::string{strip_namespace(it->get_ref<const std::string&>())};
    std::string out = name + "(";
    if (auto p = item.find("parameters"); p != item.end() && p->is_object()) {
        bool first = true;
        for (const auto& [k, v] : p->items()) {
            if (k == "display_description") continue;
            if (!first) out += ", ";
            first = false;
            out += k + "=" + clip(v.is_string() ? v.get_ref<const std::string&>() : v.dump(), 40);
        }
    }
    return out + ")";
}

bool is_wrapped_call(const json& j) {
    if (!j.is_object()) return false;
    auto r = j.find("recipient_name");
    return r != j.end() && r->is_string();
}

// The wrapped call's parameters as an object; a string holding JSON counts.
std::optional<json> parameters_of(const json& item) {
    auto p = item.find("parameters");
    if (p == item.end()) return std::optional<json>{std::in_place, json::object()};
    if (p->is_object()) return std::optional<json>{std::in_place, *p};
    if (p->is_string()) {
        auto parsed = json::parse(p->get_ref<const std::string&>(), nullptr, false);
        if (parsed.is_object()) return std::optional<json>{std::in_place, std::move(parsed)};
    }
    return std::nullopt;
}

bool only_keys(const json& j, std::initializer_list<std::string_view> keys) {
    for (const auto& [k, v] : j.items()) {
        bool ok = false;
        for (auto want : keys) ok = ok || k == want;
        if (!ok) return false;
    }
    return true;
}

constexpr std::string_view kSendSeparately =
    "Send every tool call as its own function call carrying only its own "
    "arguments \xe2\x80\x94 never a recipient_name/parameters wrapper or a "
    "tool_uses list inside another call's arguments. Several separate calls "
    "in one message still run together.";

} // namespace

bool unwrap_call_wrapper(std::string_view tool_name, json& args) {
    if (!args.is_object()) return false;
    const json* item = nullptr;
    if (auto t = args.find("tool_uses"); t != args.end()) {
        if (!only_keys(args, {"tool_uses"}) || !t->is_array() || t->size() != 1) return false;
        item = &(*t)[0];
    } else {
        item = &args;
    }
    if (!is_wrapped_call(*item) || !only_keys(*item, {"recipient_name", "parameters"}))
        return false;
    const auto& to = (*item)["recipient_name"].get_ref<const std::string&>();
    if (!ieq(strip_namespace(to), strip_namespace(tool_name))) return false;
    auto params = parameters_of(*item);
    if (!params) return false;
    args = std::move(*params);
    return true;
}

std::optional<std::string> call_wrapper_leak(std::string_view tool_name, const json& args) {
    if (!args.is_object()) return std::nullopt;
    const std::string tool{tool_name};

    // Shape 1 that unwrap_call_wrapper could not take: several calls, a call
    // to another tool, or a wrapper mixed with other keys.
    if (auto t = args.find("tool_uses"); t != args.end() && t->is_array()) {
        std::string calls;
        std::size_t n = 0;
        for (const auto& item : *t) {
            if (!is_wrapped_call(item)) continue;
            calls += (n++ ? ", " : "") + describe(item);
        }
        if (n > 0)
            return "This `" + tool + "` call's arguments are " + std::to_string(n)
                 + " tool calls packed into a `tool_uses` list (" + calls
                 + "), so none of them ran. " + std::string{kSendSeparately};
    }
    if (is_wrapped_call(args)) {
        const auto& to = args["recipient_name"].get_ref<const std::string&>();
        if (!ieq(strip_namespace(to), strip_namespace(tool_name)))
            return "This call is named `" + tool + "` but its arguments wrap a call to `"
                 + std::string{strip_namespace(to)} + "` (" + describe(args)
                 + "), so it did not run. " + std::string{kSendSeparately};
        return "This `" + tool + "` call's arguments are a recipient_name/parameters "
               "wrapper (" + describe(args) + ") mixed with other fields, so it did "
               "not run. " + std::string{kSendSeparately};
    }

    // Shape 2: wrapper debris inside location / identifier values.
    std::string strong, weak;
    auto note = [](std::string& into, std::string_view key, std::string_view v) {
        if (!into.empty()) into += ", ";
        into += "`" + std::string{key} + "` = \"" + clip(v) + "\"";
    };
    auto inspect = [&](std::string_view key, std::string_view v) {
        if (has_structural_tail(v) || has_wrapper_text(v)) note(strong, key, v);
        else if (is_wrapper_token(v)) note(weak, key, v);
    };
    for (auto key : kLocationKeys) {
        auto it = args.find(std::string{key});
        if (it != args.end() && it->is_string())
            inspect(key, it->get_ref<const std::string&>());
    }
    for (auto key : kLocationListKeys) {
        auto it = args.find(std::string{key});
        if (it == args.end() || !it->is_array()) continue;
        for (const auto& v : *it)
            if (v.is_string()) inspect(key, v.get_ref<const std::string&>());
    }
    if (strong.empty()) return std::nullopt;
    if (!weak.empty()) strong += ", " + weak;
    return "These `" + tool + "` arguments contain pieces of several tool calls "
           "run together (" + strong + "), so the call was not run \xe2\x80\x94 those "
           "are not real " + (contains(strong, "symbol") ? "paths or symbols" : "paths")
         + ". " + std::string{kSendSeparately};
}

std::string raw_call_wrapper_hint(std::string_view raw) {
    if (!contains(raw, "\"recipient_name\"") && !contains(raw, "multi_tool_use.")
        && !contains(raw, "\"tool_uses\""))
        return {};
    return "These arguments look like several tool calls packed into one "
           "(a recipient_name/parameters wrapper). " + std::string{kSendSeparately};
}

} // namespace agentty::tools::util
