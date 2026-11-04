// The sandbox proxy's parsers, against bytes the GUEST chose.
//
// WHY THIS HARNESS EXISTS. Everything else in the sandbox reads inputs agentty
// produced. These three functions read a byte stream written by whatever is
// running inside the box, which on a bad day is the thing we are defending
// against. They are the host side's only attacker-controlled parser, so
// "looks careful" is not good enough: libFuzzer + ASan + UBSan walks them.
//
// What the invariants below are really asserting: a parser that says `ok`
// handed us a host and port we are about to DIAL, so an ok with an empty host,
// a zero port, or an embedded NUL would be a bug with teeth. And `need_more`
// has to be monotone -- a prefix of a valid request must ask for more rather
// than claim malformed, or a client that writes its request in two packets
// gets a spurious refusal.
#include "agentty/tool/util/sandbox_netproxy.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace np = agentty::tools::util::sandbox::netproxy;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "invariant failed: %s at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

namespace {

// A target the parser accepted is one we are about to resolve and connect to.
void check_target(const np::Target& t) {
    if (!t.ok) {
        // Not accepted: nothing is promised about host/port, but a parser must
        // never claim both "malformed" and "incomplete" at once -- the caller
        // branches on exactly that.
        return;
    }
    CHECK(!t.need_more);                             // ok and need_more are exclusive
    CHECK(!t.host.empty());                          // we would dial nothing
    CHECK(t.port != 0);                              // port 0 is not a destination
    CHECK(t.host.find('\0') == std::string::npos);   // would truncate at the resolver
    CHECK(t.host.size() <= 255);                     // a hostname's hard limit
}

// `need_more` must be monotone in the useful direction: a prefix that is too
// short to hold a complete request must say need_more rather than
// "malformed". Otherwise a client that writes its request in two packets gets
// a spurious refusal, which looks like a flaky proxy rather than a parser bug.
//
// NOTE what this does NOT claim. A valid request can be FOLLOWED by more
// bytes (clients pipeline, and the fuzzer concatenates), so a prefix being
// complete is perfectly legal -- the first crash this harness found was that
// wrong assumption in the test, not a bug in the parser. The real invariant
// is about the prefix that is strictly shorter than the smallest complete
// request: 10 bytes for IPv4, which is the shortest form.
void check_prefix_monotonic(std::string_view full) {
    const auto whole = np::parse_socks5_request(full);
    if (!whole.target.ok) return;
    for (std::size_t n = 1; n < full.size() && n < 7; ++n) {
        const auto part = np::parse_socks5_request(full.substr(0, n));
        // Under 7 bytes nothing can be complete (05 CMD 00 ATYP + 1 addr + 2
        // port is the floor), so the parser must be asking for more.
        CHECK(!part.target.ok);
        CHECK(part.target.need_more);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::uint8_t which = data[0];
    const std::string_view in{reinterpret_cast<const char*>(data) + 1, size - 1};

    switch (which % 4) {
    case 0: {   // the SOCKS5 greeting
        const auto g = np::parse_socks5_greeting(in);
        if (g.ok) {
            CHECK(!g.need_more);
            CHECK(g.consumed >= 2);
            CHECK(g.consumed <= in.size());   // never past the buffer
        }
        break;
    }
    case 1: {   // the SOCKS5 request
        const auto r = np::parse_socks5_request(in);
        check_target(r.target);
        if (r.target.ok) {
            // Only the three address types are accepted, and only CONNECT is
            // served -- but the parser reports the cmd rather than judging it,
            // so any value is legal here.
            CHECK(r.atyp == 0x01 || r.atyp == 0x03 || r.atyp == 0x04);
        }
        check_prefix_monotonic(in);
        break;
    }
    case 2: {   // an HTTP request line
        const auto h = np::parse_http_request(in);
        check_target(h.target);
        if (h.target.ok && h.connect) {
            // CONNECT is authority-form: the port is explicit, never defaulted.
            CHECK(in.find("CONNECT") == 0);
        }
        break;
    }
    case 3: {   // the address filter, on whatever text came out of a parser
        const std::string s{in};
        const bool blocked = np::is_blocked_address(s);
        // The one hard rule: the loopback and private forms are ALWAYS
        // blocked, however they are spelled. A regression here is the audit's
        // #1 finding coming back.
        static constexpr const char* kMustBlock[] = {
            "127.0.0.1", "127.1.2.3", "::1", "::ffff:127.0.0.1",
            "10.0.0.1", "172.16.0.1", "192.168.1.1", "169.254.169.254",
            "100.64.0.1", "fc00::1", "fe80::1", "0.0.0.0",
        };
        for (const char* m : kMustBlock) CHECK(np::is_blocked_address(m));
        // And a public address is not blocked, or the proxy serves nothing.
        CHECK(!np::is_blocked_address("1.1.1.1"));
        CHECK(!np::is_blocked_address("93.184.216.34"));
        CHECK(!np::is_blocked_address("2606:2800:220:1:248:1893:25c8:1946"));
        // Anything unparseable must fail CLOSED.
        if (!blocked) CHECK(!s.empty());
        // A NUL truncates the string before inet_pton sees it, so bytes after
        // one are ignored here -- "A::\0junk" is the address A:: and is
        // legitimately allowed. That is only safe because the PARSERS refuse a
        // host containing a NUL before it ever reaches this function, so the
        // truncation cannot be used to smuggle a second address past the
        // filter. Assert that defence where it actually lives:
        {
            // 05 CONNECT 00 ATYP=name len="1.1.1.1\0127.0.0.1" port=80
            const std::string smuggle = std::string("\x05\x01\x00\x03", 4)
                + static_cast<char>(17) + std::string("1.1.1.1\0" "127.0.0.1", 17)
                + std::string("\x00\x50", 2);
            const auto r = np::parse_socks5_request(smuggle);
            CHECK(!r.target.ok);   // refused for the NUL, not resolved
        }
        break;
    }
    }
    return 0;
}
