// agentty::tools::schema — see schema.hpp.

#include "agentty/tool/schema.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <vector>

namespace agentty::tools::schema {

using json = nlohmann::json;

namespace {

bool key_ok(std::string_view k) {
    if (k.empty() || k.size() > 64) return false;
    for (unsigned char c : k)
        if (!std::isalnum(c) && c != '_' && c != '.' && c != '-') return false;
    return true;
}

bool type_name_ok(const json& t) {
    static const std::set<std::string, std::less<>> kTypes{
        "string", "number", "integer", "boolean", "object", "array", "null"};
    return t.is_string() && kTypes.contains(t.get<std::string>());
}

// The 2020-12 meta-schema checks keyword VALUE TYPES; measured, those are
// what the live API rejects (a semantic mistake like a dangling $ref or an
// unknown format passes). Walk every subschema and check the keywords whose
// value shape the meta-schema constrains.
std::string walk(const json& s, const std::string& at, int depth) {
    if (depth > 64) return "schema nests too deep";
    if (s.is_boolean()) return {};
    if (!s.is_object()) return at + " is not a schema object";

    auto where = [&](std::string_view key) { return at.empty() ? std::string{key} : at + "." + std::string{key}; };

    if (auto it = s.find("type"); it != s.end()) {
        if (it->is_array()) {
            if (it->empty()) return where("type") + " is an empty list";
            for (const auto& t : *it)
                if (!type_name_ok(t)) return where("type") + " has an unknown type";
        } else if (!type_name_ok(*it)) {
            return where("type") + " is not a JSON type";
        }
    }
    for (const char* k : {"description", "title", "pattern", "format", "$ref", "$id", "$comment"})
        if (auto it = s.find(k); it != s.end() && !it->is_string())
            return where(k) + " must be a string";
    for (const char* k : {"minLength", "maxLength", "minItems", "maxItems", "minProperties",
                          "maxProperties", "minContains", "maxContains"})
        if (auto it = s.find(k); it != s.end()) {
            if (!it->is_number_integer() && !it->is_number_unsigned()) return where(k) + " must be an integer";
            if (it->is_number_integer() && it->get<std::int64_t>() < 0) return where(k) + " must not be negative";
        }
    for (const char* k : {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf"})
        if (auto it = s.find(k); it != s.end() && !it->is_number()) return where(k) + " must be a number";
    if (auto it = s.find("multipleOf"); it != s.end() && it->is_number() && it->get<double>() <= 0)
        return where("multipleOf") + " must be positive";
    if (auto it = s.find("enum"); it != s.end() && !it->is_array()) return where("enum") + " must be a list";
    if (auto it = s.find("required"); it != s.end()) {
        // The live API accepts a non-list `required` (draft-4 style), so only
        // a list of non-strings is a real problem.
        if (it->is_array())
            for (const auto& r : *it)
                if (!r.is_string()) return where("required") + " must list names";
    }
    for (const char* k : {"uniqueItems", "readOnly", "writeOnly", "deprecated"})
        if (auto it = s.find(k); it != s.end() && !it->is_boolean()) return where(k) + " must be true or false";

    // Maps of subschemas.
    for (const char* k : {"properties", "patternProperties", "$defs", "definitions", "dependentSchemas"}) {
        auto it = s.find(k);
        if (it == s.end()) continue;
        if (!it->is_object()) return where(k) + " must be an object";
        for (const auto& [name, sub] : it->items())
            if (auto e = walk(sub, where(k) + "." + name, depth + 1); !e.empty()) return e;
    }
    // Single subschemas.
    for (const char* k : {"items", "additionalProperties", "not", "if", "then", "else", "contains",
                          "propertyNames", "unevaluatedItems", "unevaluatedProperties",
                          "additionalItems"}) {
        auto it = s.find(k);
        if (it == s.end()) continue;
        // `items` as a list is draft-4 tuple form; 2020-12 calls it prefixItems.
        if (it->is_array() && std::string_view{k} == "items") {
            for (const auto& sub : *it)
                if (auto e = walk(sub, where(k), depth + 1); !e.empty()) return e;
            continue;
        }
        if (!it->is_object() && !it->is_boolean()) return where(k) + " must be a schema";
        if (auto e = walk(*it, where(k), depth + 1); !e.empty()) return e;
    }
    // Lists of subschemas.
    for (const char* k : {"anyOf", "oneOf", "allOf", "prefixItems"}) {
        auto it = s.find(k);
        if (it == s.end()) continue;
        if (!it->is_array() || it->empty()) return where(k) + " must be a non-empty list";
        for (const auto& sub : *it)
            if (auto e = walk(sub, where(k), depth + 1); !e.empty()) return e;
    }
    return {};
}

// ── normalize helpers ──────────────────────────────────────────────────

// Inline local `#/$defs/X` and `#/definitions/X` refs. A ref that loops back
// on itself is cut to `{}` (any value) at the second visit; a dangling one
// becomes `{}` too, since there is nothing to inline.
json inline_refs(const json& node, const json& root, std::vector<std::string>& stack) {
    if (node.is_array()) {
        json out = json::array();
        for (const auto& e : node) out.push_back(inline_refs(e, root, stack));
        return out;
    }
    if (!node.is_object()) return node;
    if (auto r = node.find("$ref"); r != node.end() && r->is_string()) {
        const std::string ref = *r;
        std::string_view v = ref;
        const json* target = nullptr;
        for (std::string_view pre : {std::string_view{"#/$defs/"}, std::string_view{"#/definitions/"}}) {
            if (!v.starts_with(pre)) continue;
            const auto table = pre == "#/$defs/" ? "$defs" : "definitions";
            if (auto t = root.find(table); t != root.end() && t->is_object())
                if (auto d = t->find(std::string{v.substr(pre.size())}); d != t->end()) target = &*d;
        }
        if (!target || std::find(stack.begin(), stack.end(), ref) != stack.end()) {
            json any = json::object();
            if (auto d = node.find("description"); d != node.end()) any["description"] = *d;
            return any;
        }
        stack.push_back(ref);
        json expanded = inline_refs(*target, root, stack);
        stack.pop_back();
        // Sibling keywords next to $ref (2020-12 allows them) are kept.
        for (const auto& [k, v2] : node.items())
            if (k != "$ref" && expanded.is_object()) expanded[k] = inline_refs(v2, root, stack);
        return expanded;
    }
    json out = json::object();
    for (const auto& [k, v] : node.items()) {
        if (k == "$defs" || k == "definitions") continue;   // inlined above
        out[k] = inline_refs(v, root, stack);
    }
    return out;
}

// `type: ["string","null"]` → `type: "string"` + `nullable: true`, the form
// Gemini and strict OpenAI-compatible servers accept. Other multi-type
// arrays keep only the first non-null type (an argument rarely needs more).
void collapse_type_arrays(json& node) {
    if (node.is_array()) { for (auto& e : node) collapse_type_arrays(e); return; }
    if (!node.is_object()) return;
    if (auto t = node.find("type"); t != node.end() && t->is_array()) {
        bool null = false;
        std::string first;
        for (const auto& x : *t) {
            if (!x.is_string()) continue;
            if (x == "null") null = true;
            else if (first.empty()) first = x.get<std::string>();
        }
        if (first.empty()) node.erase("type");
        else *t = first;
        if (null) node["nullable"] = true;
    }
    for (auto& [k, v] : node.items())
        if (k != "enum" && k != "const" && k != "default" && k != "examples") collapse_type_arrays(v);
}

// Old-draft spellings that strict validators (Gemini, OpenAI strict mode)
// reject, rewritten to 2020-12 the way OpenCode does:
//   required not a list, or naming undeclared properties
//   boolean exclusiveMinimum/Maximum (draft-04)
//   items as a list (draft-07 tuples) -> prefixItems
void modernize(json& node) {
    if (node.is_array()) { for (auto& e : node) modernize(e); return; }
    if (!node.is_object()) return;
    if (auto r = node.find("required"); r != node.end()) {
        json list = json::array();
        const json* props = node.contains("properties") && node["properties"].is_object()
                                ? &node["properties"] : nullptr;
        auto keep = [&](const json& n) { return n.is_string() && (!props || props->contains(n.get<std::string>())); };
        if (r->is_string()) { if (keep(*r)) list.push_back(*r); }
        else if (r->is_array()) for (const auto& n : *r) if (keep(n)) list.push_back(n);
        if (list.empty()) node.erase("required"); else node["required"] = list;
    }
    for (const auto& [ex, bound] : {std::pair{"exclusiveMinimum", "minimum"},
                                    std::pair{"exclusiveMaximum", "maximum"}}) {
        auto e = node.find(ex);
        if (e == node.end() || !e->is_boolean()) continue;
        const bool on = e->get<bool>();
        if (on && node.contains(bound) && node[bound].is_number()) {
            node[ex] = node[bound];
            node.erase(bound);
        } else {
            node.erase(ex);
        }
    }
    if (auto it = node.find("items"); it != node.end() && it->is_array()) {
        json tuple = *it;
        node.erase("items");
        node["prefixItems"] = tuple;
        if (auto a = node.find("additionalItems"); a != node.end()) {
            node["items"] = *a;
            node.erase("additionalItems");
        }
    }
    for (auto& [k, v] : node.items())
        if (k != "enum" && k != "const" && k != "default" && k != "examples") modernize(v);
}

// Drop descriptions below the top-level parameters. A parameter's own
// description (properties.X.description) is what the model needs most and
// stays; anything nested under it goes first.
void strip_nested_descriptions(json& node, int depth) {
    if (node.is_array()) { for (auto& e : node) strip_nested_descriptions(e, depth); return; }
    if (!node.is_object()) return;
    // depth: 0 = root, 1 = a top-level parameter, 2+ = inside one.
    if (depth >= 2) { node.erase("description"); node.erase("title"); node.erase("examples"); }
    for (auto& [k, v] : node.items()) {
        if (k == "enum" || k == "const" || k == "default") continue;
        if (k == "properties" && v.is_object()) {
            for (auto& [_, sub] : v.items()) strip_nested_descriptions(sub, depth + 1);
        } else if (v.is_object() || v.is_array()) {
            strip_nested_descriptions(v, depth == 0 ? 0 : depth + 1);
        }
    }
}

// Last resort: top-level parameter descriptions shortened to one sentence.
void shorten_descriptions(json& node) {
    if (auto p = node.find("properties"); p != node.end() && p->is_object())
        for (auto& [_, sub] : p->items())
            if (auto d = sub.find("description"); d != sub.end() && d->is_string()) {
                std::string s = *d;
                if (const auto dot = s.find(". "); dot != std::string::npos) s.resize(dot + 1);
                if (s.size() > 120) s = s.substr(0, 117) + "...";
                *d = s;
            }
}

void collapse_deep_objects(json& node, int depth) {
    if (!node.is_object()) { if (node.is_array()) for (auto& e : node) collapse_deep_objects(e, depth); return; }
    if (depth >= 4 && node.value("type", std::string{}) == "object" && node.contains("properties")) {
        node.erase("properties");
        node.erase("required");
        node.erase("additionalProperties");
        return;
    }
    for (auto& [k, v] : node.items())
        if (k != "enum" && k != "const" && k != "default") collapse_deep_objects(v, depth + 1);
}

std::uint32_t fnv1a(std::string_view s) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

std::string sanitize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back((std::isalnum(c) || c == '_' || c == '-') ? static_cast<char>(c) : '_');
    return out;
}

} // namespace

std::string check(const json& s) {
    if (!s.is_object()) return "the input schema is not an object";
    // Top-level property NAMES are a separate check on the live API; nested
    // ones pass (measured), so only the top level is enforced.
    if (auto p = s.find("properties"); p != s.end() && p->is_object())
        for (const auto& [name, _] : p->items())
            if (!key_ok(name))
                return "parameter '" + name + "' has characters or a length providers reject";
    return walk(s, "", 0);
}

Normalized normalize(json s, std::size_t compact_bytes) {
    Normalized out;
    if (!s.is_object()) s = json::object();

    // $ref → inline (Gemini rejects $ref outside a few safe shapes; strict
    // OpenAI-compatible servers often reject it entirely).
    if (s.contains("$defs") || s.contains("definitions") || s.dump().find("\"$ref\"") != std::string::npos) {
        std::vector<std::string> stack;
        s = inline_refs(s, s, stack);
    }

    // Root union → one object. The Claude API rejects anyOf/oneOf/allOf at
    // the root; merge every branch's properties and describe which groups
    // go together, the way Claude Code does.
    for (const char* k : {"allOf", "anyOf", "oneOf"}) {
        auto it = s.find(k);
        if (it == s.end() || !it->is_array()) continue;
        const bool all = std::string_view{k} == "allOf";
        json props = s.value("properties", json::object());
        std::vector<std::string> req = s.value("required", std::vector<std::string>{});
        std::vector<std::string> groups;
        for (const auto& br : *it) {
            if (!br.is_object()) continue;
            std::vector<std::string> names;
            if (auto bp = br.find("properties"); bp != br.end() && bp->is_object())
                for (const auto& [n, v] : bp->items()) {
                    if (!props.contains(n)) props[n] = v;
                    names.push_back(n);
                }
            if (auto br_req = br.find("required"); br_req != br.end() && br_req->is_array()) {
                std::vector<std::string> r;
                for (const auto& x : *br_req) if (x.is_string()) r.push_back(x);
                if (all) req.insert(req.end(), r.begin(), r.end());
                else if (!r.empty()) {
                    std::string g;
                    for (const auto& n : r) g += (g.empty() ? "" : ", ") + n;
                    groups.push_back(g);
                }
            }
        }
        s.erase(k);
        s["type"] = "object";
        s["properties"] = props;
        std::sort(req.begin(), req.end());
        req.erase(std::unique(req.begin(), req.end()), req.end());
        if (!req.empty()) s["required"] = req; else s.erase("required");
        if (!all && groups.size() > 1) {
            std::string note = "Pass exactly one of these parameter groups: ";
            for (std::size_t i = 0; i < groups.size(); ++i)
                note += (i ? "; " : "") + std::string{"("} + groups[i] + ")";
            out.note = note + ".";
        }
    }

    collapse_type_arrays(s);
    modernize(s);
    if (!s.contains("type")) s["type"] = "object";
    if (!s.contains("properties")) s["properties"] = json::object();
    s.erase("$schema");   // a declared draft-07 dialect confuses some servers

    // Compact a schema past the budget: nested descriptions first, then
    // deeply nested object shapes (the server still validates the args).
    if (compact_bytes > 0 && s.dump().size() > compact_bytes) {
        strip_nested_descriptions(s, 0);
        if (s.dump().size() > compact_bytes) shorten_descriptions(s);
        if (s.dump().size() > compact_bytes) collapse_deep_objects(s, 0);
    }
    out.schema = std::move(s);
    return out;
}

std::string wire_name(std::string_view server, std::string_view tool,
                      std::unordered_set<std::string>& taken) {
    const std::string raw = std::string{server} + "__" + std::string{tool};
    std::string name = "mcp__" + sanitize(server) + "__" + sanitize(tool);
    if (name.size() <= kMaxNameLen && !taken.contains(name)) {
        taken.insert(name);
        return name;
    }
    // Too long, or two raw names sanitised to the same thing: shorten and
    // add a hash of the RAW name, so it's unique and stable across runs.
    for (unsigned attempt = 0;; ++attempt) {
        char hex[12];
        const std::string key = attempt == 0 ? raw : raw + '\0' + std::to_string(attempt);
        std::snprintf(hex, sizeof(hex), "_%08x", fnv1a(key));
        std::string prefix = "mcp__" + sanitize(server).substr(0, 20) + "__";
        const std::size_t room = kMaxNameLen - prefix.size() - 9;
        std::string candidate = prefix + sanitize(tool).substr(0, room) + hex;
        if (!taken.contains(candidate)) {
            taken.insert(candidate);
            return candidate;
        }
    }
}

} // namespace agentty::tools::schema
