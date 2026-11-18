// The sandbox proxy's host allowlist: the pure name matcher, pinned case by
// case. The proxy enforces it on the name the guest ASKED for, before it
// resolves anything, so every rule here is the whole rule -- there is no DNS
// step behind it that could rescue a wrong match.
#include "agtest.hpp"

#include "agentty/tool/util/sandbox_netproxy.hpp"

#include <string>
#include <vector>

namespace np = agentty::tools::util::sandbox::netproxy;

TEST_CASE("netproxy: an empty allowlist narrows nothing") {
    const std::vector<std::string> none;
    CHECK(np::host_allowed("github.com", none));
    CHECK(np::host_allowed("anything.example", none));
    // literals too: with no list the ADDRESS filter is the only rule, and it
    // runs after this one.
    CHECK(np::host_allowed("1.1.1.1", none));
}

TEST_CASE("netproxy: exact entries match exactly that name") {
    const std::vector<std::string> l{"github.com"};
    CHECK(np::host_allowed("github.com", l));
    CHECK(np::host_allowed("GitHub.COM", l));           // DNS is case-blind
    CHECK(np::host_allowed("github.com.", l));          // FQDN trailing dot
    CHECK_FALSE(np::host_allowed("api.github.com", l)); // no implied subdomains
    CHECK_FALSE(np::host_allowed("github.com.evil.io", l));
    CHECK_FALSE(np::host_allowed("evilgithub.com", l));
    CHECK_FALSE(np::host_allowed("", l));
}

TEST_CASE("netproxy: *.x matches strictly under x, not x, not lookalikes") {
    const std::vector<std::string> l{"*.githubusercontent.com"};
    CHECK(np::host_allowed("raw.githubusercontent.com", l));
    CHECK(np::host_allowed("a.b.githubusercontent.com", l));
    CHECK(np::host_allowed("RAW.GithubUserContent.com", l));
    // the apex is NOT included: say both if you mean both.
    CHECK_FALSE(np::host_allowed("githubusercontent.com", l));
    // the leading dot is what makes it a label boundary.
    CHECK_FALSE(np::host_allowed("evilgithubusercontent.com", l));
    CHECK_FALSE(np::host_allowed("githubusercontent.com.evil.io", l));
}

TEST_CASE("netproxy: IP literals are refused whenever a list is set") {
    // an allowlist of NAMES cannot vouch for an address, and letting literals
    // through would make every name rule bypassable with `curl 140.82.x.x`.
    const std::vector<std::string> l{"github.com", "*.github.com"};
    CHECK_FALSE(np::host_allowed("140.82.112.3", l));
    CHECK_FALSE(np::host_allowed("2606:50c0:8000::153", l));
    CHECK_FALSE(np::host_allowed("[2606:50c0:8000::153]", l));
    CHECK_FALSE(np::host_allowed("127.0.0.1", l));
    // even when someone lists one: the entry is a name rule, and a literal
    // host never takes the name path.
    const std::vector<std::string> lit{"1.1.1.1"};
    CHECK_FALSE(np::host_allowed("1.1.1.1", lit));
}

TEST_CASE("netproxy: several entries are a union") {
    const std::vector<std::string> l{"github.com", "*.github.com", "registry.npmjs.org", "pypi.org."};
    CHECK(np::host_allowed("github.com", l));
    CHECK(np::host_allowed("api.github.com", l));
    CHECK(np::host_allowed("registry.npmjs.org", l));
    CHECK(np::host_allowed("pypi.org", l));             // trailing dot on the entry
    CHECK_FALSE(np::host_allowed("npmjs.org", l));
    CHECK_FALSE(np::host_allowed("files.pythonhosted.org", l));
}

// ── normalize_host_entry: what the settings pane accepts, and as what ─────

namespace {
std::string norm(std::string_view s) { return np::normalize_host_entry(s).value; }
bool rejected(std::string_view s) {
    const auto e = np::normalize_host_entry(s);
    return e.value.empty() && !e.why.empty();
}
}  // namespace

TEST_CASE("netproxy: pasted URLs and common spellings are trimmed to the host") {
    CHECK(norm("github.com") == "github.com");
    CHECK(norm("  GitHub.COM  ") == "github.com");
    CHECK(norm("https://github.com/1ay1/agentty?tab=readme#x") == "github.com");
    CHECK(norm("http://user:pw@registry.npmjs.org:8080/pkg") == "registry.npmjs.org");
    CHECK(norm("github.com:443") == "github.com");
    CHECK(norm("github.com.") == "github.com");
    // the curl / no_proxy spelling of "any subdomain".
    CHECK(norm(".github.com") == "*.github.com");
    CHECK(norm("*.GitHub.com") == "*.github.com");
    CHECK(norm("xn--bcher-kva.example") == "xn--bcher-kva.example");   // punycode is a name
}

TEST_CASE("netproxy: entries that cannot mean a host are rejected, with a reason") {
    CHECK(rejected(""));
    CHECK(rejected("   "));
    CHECK(rejected("*"));                     // would allow everything
    CHECK(rejected("*.com"));                 // a whole TLD
    CHECK(rejected("api.*.github.com"));      // wildcard not leftmost
    CHECK(rejected("git*hub.com"));
    CHECK(rejected("140.82.112.3"));          // addresses are never allowlisted
    CHECK(rejected("https://140.82.112.3/"));
    CHECK(rejected("[2606:50c0::153]"));
    CHECK(rejected("2606:50c0::153"));
    CHECK(rejected("github .com"));
    CHECK(rejected("github..com"));
    CHECK(rejected("github.com:http"));
    CHECK(rejected("https://"));
    CHECK(rejected("gith\xc3\xbcb.com"));      // raw UTF-8: punycode it first
    CHECK(rejected(std::string(260, 'a') + ".com"));
}

TEST_CASE("netproxy: a normalised entry matches what the raw one meant") {
    // the round trip the pane relies on: accept raw -> store canonical ->
    // the proxy matches the host the user had in mind.
    for (const char* raw : {"https://GitHub.com/x", "github.com:443", "github.com."}) {
        const std::vector<std::string> l{norm(raw)};
        CHECK(np::host_allowed("github.com", l));
        CHECK_FALSE(np::host_allowed("api.github.com", l));
    }
    const std::vector<std::string> w{norm(".github.com")};
    CHECK(np::host_allowed("api.github.com", w));
    CHECK_FALSE(np::host_allowed("github.com", w));
}
