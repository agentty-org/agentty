// agentty::tools::plan — see tool_plan.hpp.

#include "agentty/tool/tool_plan.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

namespace agentty::tools::plan {

std::string_view to_string(Loading l) noexcept {
    switch (l) {
        case Loading::Auto:     return "auto";
        case Loading::All:      return "all";
        case Loading::OnDemand: return "on_demand";
    }
    return "auto";
}

Loading loading_from(std::string_view s) noexcept {
    if (s == "all") return Loading::All;
    if (s == "on_demand" || s == "on-demand" || s == "ondemand") return Loading::OnDemand;
    return Loading::Auto;
}

std::size_t schema_tokens(const ToolDef& t) {
    std::size_t bytes = t.name.value.size() + t.description.size() + 16;
    if (!t.input_schema.is_null()) bytes += t.input_schema.dump().size();
    return static_cast<std::size_t>(std::ceil(static_cast<double>(bytes) / 3.5));
}

namespace {

// Always on the wire: the core toolset, what the user pinned, and the few
// broker tools that carry their own always_expose (resources, prompts).
bool always_loaded(const ToolDef& t) {
    return t.origin == ToolOrigin::Native || t.always_expose;
}

} // namespace

void Loaded::touch(std::string_view name) {
    std::string n{name};
    if (!names.insert(n).second)
        order.erase(std::find(order.begin(), order.end(), n));
    order.push_back(std::move(n));
}

Plan make_plan(const std::vector<ToolDef>& catalog,
               const Loaded& loaded,
               const ToolDef* search, Loading mode, const Limits& limits) {
    Plan out;
    const std::size_t window = limits.context_window ? limits.context_window : 128'000;
    const std::size_t budget = static_cast<std::size_t>(
        static_cast<double>(window) * std::clamp(limits.schema_share, 0.0, 1.0));
    const std::size_t cap = limits.max_tools;

    std::vector<char> load(catalog.size(), 0);
    std::size_t count = 0, tokens = 0;
    // Auto's budget is for PLUGIN schemas only, like Claude Code's tool-search
    // threshold: the natives are the core toolset and always ride, so
    // charging them would leave no room for any plugin on a small window.
    std::size_t plugin_tokens = 0;
    auto take = [&](std::size_t i) {
        load[i] = 1;
        ++count;
        const auto cost = schema_tokens(catalog[i]);
        tokens += cost;
        if (catalog[i].origin != ToolOrigin::Native) plugin_tokens += cost;
    };

    // 1. Must-load: natives and pins.
    for (std::size_t i = 0; i < catalog.size(); ++i)
        if (catalog[i].advertise && always_loaded(catalog[i])) take(i);

    // With a hard cap, the fill can't know in advance whether something will
    // be left over, so it stops one short when anything was or would be:
    // that last slot is tool_search's.
    bool skipped = false;
    auto fits_cap = [&](std::size_t left_after_this) {
        if (cap == 0) return true;
        const std::size_t reserve = ((skipped || left_after_this > 0) && search) ? 1 : 0;
        return count + 1 + reserve <= cap;
    };
    auto unloaded = [&] {
        std::size_t n = 0;
        for (std::size_t i = 0; i < catalog.size(); ++i)
            if (catalog[i].advertise && !load[i]) ++n;
        return n;
    };

    // 2. What this thread already used, newest first: past the schema budget
    //    but never past the cap. A long thread can touch more tools than the
    //    cap allows; the oldest go back on demand.
    if (!loaded.order.empty()) {
        std::unordered_map<std::string_view, std::size_t> by_name;
        for (std::size_t i = 0; i < catalog.size(); ++i)
            if (catalog[i].advertise && !load[i]) by_name.emplace(catalog[i].name.value, i);
        std::size_t remaining = unloaded();
        for (auto n = loaded.order.rbegin(); n != loaded.order.rend(); ++n) {
            const auto it = by_name.find(*n);
            if (it == by_name.end() || load[it->second]) continue;
            --remaining;
            if (fits_cap(remaining)) take(it->second);
            else skipped = true;
        }
    }

    // 3. The rest, by mode. Catalog order is the fill order, so the same
    //    catalog and the same loaded set always give the same wire.
    if (mode != Loading::OnDemand) {
        std::size_t remaining = unloaded();
        for (std::size_t i = 0; i < catalog.size(); ++i) {
            const auto& t = catalog[i];
            if (!t.advertise || load[i]) continue;
            --remaining;   // tools still to consider after this one
            const std::size_t cost = schema_tokens(t);
            const bool in_budget = mode == Loading::All || plugin_tokens + cost <= budget;
            if (in_budget && fits_cap(remaining)) {
                take(i);
            } else {
                skipped = true;
                if (mode == Loading::All) out.capped_by_provider = true;
            }
        }
    }

    // 4. Emit in catalog order.
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& t = catalog[i];
        if (!t.advertise) continue;
        if (load[i]) {
            out.wire.push_back({&t, false});
            ++out.loaded;
        } else {
            ++out.on_demand;
            if (limits.discovery == Discovery::DeferredReferences)
                out.wire.push_back({&t, true});
        }
    }
    out.schema_tokens = tokens;
    if (out.on_demand > 0 && search) {
        out.wire.push_back({search, false});
        out.search = true;
    }
    return out;
}

// ── Search ───────────────────────────────────────────────────────────────

namespace {

// Lowercase words, with identifiers split: snake_case, kebab-case,
// camelCase, digits, and the mcp__server__tool separator all become breaks.
std::vector<std::string> tokenize(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (cur.size() > 1) out.push_back(cur);
        cur.clear();
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {             // keep UTF-8 runs whole (non-English names)
            cur.push_back(static_cast<char>(c));
            continue;
        }
        if (!std::isalnum(c)) { flush(); continue; }
        const bool upper = std::isupper(c);
        if (upper && !cur.empty()) {
            const auto prev = static_cast<unsigned char>(s[i - 1]);
            const bool next_lower = i + 1 < s.size()
                && std::islower(static_cast<unsigned char>(s[i + 1]));
            if (std::islower(prev) || (std::isupper(prev) && next_lower)) flush();
        }
        cur.push_back(static_cast<char>(std::tolower(c)));
    }
    flush();
    return out;
}

// Light stemming so "functions"/"function", "decompiled"/"decompile" meet.
std::string stem(std::string w) {
    auto strip = [&](std::string_view suf, std::size_t keep) {
        if (w.size() > suf.size() + keep && w.ends_with(suf)) {
            w.resize(w.size() - suf.size());
            return true;
        }
        return false;
    };
    if (!strip("ing", 3) && !strip("ies", 2) && !strip("ed", 3) && !strip("es", 3))
        strip("s", 3);
    return w;
}

// Damerau–Levenshtein distance, bounded: returns > max as soon as it can.
std::size_t edit_distance(std::string_view a, std::string_view b, std::size_t max) {
    if (a.size() > b.size() + max || b.size() > a.size() + max) return max + 1;
    std::vector<std::size_t> pp(b.size() + 1), p(b.size() + 1), c(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) p[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        c[0] = i;
        std::size_t row_min = c[0];
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            c[j] = std::min({p[j] + 1, c[j - 1] + 1, p[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                c[j] = std::min(c[j], pp[j - 2] + 1);
            row_min = std::min(row_min, c[j]);
        }
        if (row_min > max) return max + 1;
        std::swap(pp, p);
        std::swap(p, c);
    }
    return p[b.size()];
}

struct Doc {
    const ToolDef* tool;
    std::unordered_map<std::string, double> tf;   // field-weighted term counts
    double len = 0;
};

} // namespace

std::vector<Hit> search(const std::vector<ToolDef>& catalog,
                        std::string_view query, std::size_t k) {
    std::vector<Hit> out;
    if (k == 0) return out;

    // select:a,b — exact names (bare or full), in the order given.
    if (query.starts_with("select:")) {
        std::string_view rest = query.substr(7);
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            std::string_view name = rest.substr(0, comma);
            while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
            while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
            for (const auto& t : catalog) {
                if (t.origin == ToolOrigin::Native) continue;
                const std::string_view full = t.name.value;
                // The wire name may be shortened; the server's own name isn't.
                const std::string_view bare = !t.origin_tool.empty() ? std::string_view{t.origin_tool}
                    : (full.rfind("__") == std::string_view::npos ? full
                                                                   : full.substr(full.rfind("__") + 2));
                if (!name.empty() && (full == name || bare == name)) {
                    out.push_back({&t, 1.0});
                    break;
                }
            }
            if (comma == std::string_view::npos || out.size() >= k) break;
            rest.remove_prefix(comma + 1);
        }
        return out;
    }

    // Index the searchable tools. Field weights: the tool's own name is the
    // strongest signal, then its server, then the description and argument
    // names (which carry the verbs and nouns people actually type).
    std::vector<Doc> docs;
    for (const auto& t : catalog) {
        if (t.origin == ToolOrigin::Native || !t.advertise) continue;
        Doc d{&t, {}, 0};
        auto add = [&](std::string_view text, double w) {
            for (auto& tok : tokenize(text)) {
                d.tf[stem(std::move(tok))] += w;
                d.len += w;
            }
        };
        add(t.name.value, 3.0);
        if (!t.origin_tool.empty()) add(t.origin_tool, 3.0);   // unshortened
        add(t.origin_id, 1.5);
        add(t.description, 1.0);
        if (t.input_schema.is_object() && t.input_schema.contains("properties")
            && t.input_schema["properties"].is_object())
            for (const auto& [arg, _] : t.input_schema["properties"].items()) add(arg, 1.0);
        docs.push_back(std::move(d));
    }
    if (docs.empty()) return out;

    std::vector<std::string> terms;
    for (auto& tok : tokenize(query)) terms.push_back(stem(std::move(tok)));
    // A few words people use that tool names spell differently. Added as
    // extra terms (never replacing), so an exact match still wins.
    static const std::pair<std::string_view, std::string_view> kAlso[] = {
        {"bug", "issue"},   {"ticket", "issue"},  {"pr", "pull"},
        {"caller", "xref"},  {"call", "xref"},     {"reference", "xref"},
        {"repo", "repository"}, {"msg", "message"}, {"dm", "message"},
        {"sql", "query"},    {"db", "database"},   {"url", "navigate"},
        {"log", "logs"},
        // Verbs: what the user wants done picks between same-noun tools
        // (create_issue vs get_issue vs list_issues).
        {"file", "create"},  {"open", "create"},   {"make", "create"},
        {"new", "create"},   {"add", "create"},    {"show", "get"},
        {"fetch", "get"},    {"read", "get"},      {"find", "search"},
        {"look", "search"},  {"delete", "remove"}, {"send", "post"},
        {"edit", "update"},  {"change", "update"},
    };
    for (std::size_t i = 0, n0 = terms.size(); i < n0; ++i)
        for (const auto& [from, to] : kAlso)
            if (terms[i] == from) terms.push_back(stem(std::string{to}));
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    if (terms.empty()) return out;

    // Vocabulary, for typo fallback: a query word that matches nothing is
    // mapped to the closest indexed word within a small edit distance.
    std::unordered_map<std::string, std::size_t> df;
    for (const auto& d : docs)
        for (const auto& [w, _] : d.tf) ++df[w];
    for (auto& term : terms) {
        if (df.contains(term) || term.size() < 4) continue;
        const std::size_t max = term.size() >= 8 ? 2 : 1;
        std::string best;
        std::size_t best_d = max + 1;
        for (const auto& [w, _] : df) {
            const auto dist = edit_distance(term, w, max);
            if (dist < best_d || (dist == best_d && w < best)) {
                best_d = dist;
                best = w;
            }
        }
        if (best_d <= max) term = best;
    }

    double avg = 0;
    for (const auto& d : docs) avg += d.len;
    avg /= static_cast<double>(docs.size());
    const double n = static_cast<double>(docs.size());
    constexpr double k1 = 1.2, b = 0.75;

    std::vector<Hit> scored;
    for (const auto& d : docs) {
        double s = 0;
        for (const auto& term : terms) {
            double tf = 0;
            if (auto it = d.tf.find(term); it != d.tf.end()) tf = it->second;
            else {
                // Prefix match ("decomp" → "decompile") at half weight.
                for (const auto& [w, c] : d.tf)
                    if (term.size() >= 4 && w.starts_with(term)) tf = std::max(tf, c * 0.5);
            }
            if (tf == 0) continue;
            const double dfi = static_cast<double>(df.contains(term) ? df[term] : 1);
            const double idf = std::log(1.0 + (n - dfi + 0.5) / (dfi + 0.5));
            s += idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * d.len / avg));
        }
        if (s > 0) scored.push_back({d.tool, s});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (scored.size() > k) scored.resize(k);
    return scored;
}

// ── Thread state ─────────────────────────────────────────────────────────

Loaded loaded_in(const Thread& thread) {
    return loaded_in(thread.messages);
}

Loaded loaded_in(const std::vector<Message>& messages) {
    Loaded out;
    for (const auto& m : messages) {
        for (const auto& tc : m.tool_calls) {
            if (tc.name.value != kSearchToolName) {
                out.touch(tc.name.value);
                continue;
            }
            // tool_search: its result names what it loaded, one per line.
            const auto* done = std::get_if<ToolUse::Done>(&tc.status);
            if (!done) continue;
            std::string_view text = done->output;
            while (!text.empty()) {
                const auto nl = text.find('\n');
                std::string_view line = text.substr(0, nl);
                if (line.starts_with(kLoadedMarker)) {
                    line.remove_prefix(kLoadedMarker.size());
                    if (const auto sp = line.find(' '); sp != std::string_view::npos)
                        line = line.substr(0, sp);
                    if (!line.empty()) out.touch(line);
                }
                if (nl == std::string_view::npos) break;
                text.remove_prefix(nl + 1);
            }
        }
    }
    return out;
}

} // namespace agentty::tools::plan
