// The sandbox's filtered outbound network. See the header for why.
#include "agentty/tool/util/sandbox_netproxy.hpp"

#include "agentty/util/background.hpp"
#include "agentty/util/logx.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstring>
#include <vector>

namespace agentty::tools::util::sandbox::netproxy {

namespace {

// ── the address filter ───────────────────────────────────────────────────
//
// Written over the PARSED bytes rather than the string: a textual check is
// how "127.1", "0x7f.1", "::ffff:127.0.0.1" and "2130706433" get through.
// inet_pton does the parsing, so every spelling of an address lands on the
// same four (or sixteen) bytes.

bool v4_blocked(const unsigned char b[4]) noexcept {
    // 0.0.0.0/8 — "this network". not a destination, and 0.0.0.0 itself
    // means "the local host" to connect(2).
    if (b[0] == 0) return true;
    // 127/8 — the host's own loopback. THE finding.
    if (b[0] == 127) return true;
    // 10/8, 172.16/12, 192.168/16 — RFC1918. the LAN.
    if (b[0] == 10) return true;
    if (b[0] == 172 && (b[1] & 0xf0) == 16) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    // 169.254/16 — link-local, and with it 169.254.169.254: the cloud
    // metadata endpoint that hands out instance credentials.
    if (b[0] == 169 && b[1] == 254) return true;
    // 100.64/10 — CGNAT, which is also Tailscale's range. A tailnet is a
    // private network with a different name.
    if (b[0] == 100 && (b[1] & 0xc0) == 64) return true;
    // 192.0.0/24 (IETF protocol assignments), 192.0.2/24, 198.51.100/24,
    // 203.0.113/24 (documentation), 198.18/15 (benchmarking).
    if (b[0] == 192 && b[1] == 0 && b[2] == 0) return true;
    if (b[0] == 192 && b[1] == 0 && b[2] == 2) return true;
    if (b[0] == 198 && b[1] == 51 && b[2] == 100) return true;
    if (b[0] == 203 && b[1] == 0 && b[2] == 113) return true;
    if (b[0] == 198 && (b[1] & 0xfe) == 18) return true;
    // 224/4 multicast and 240/4 reserved: not unicast destinations.
    if ((b[0] & 0xf0) == 224) return true;
    if ((b[0] & 0xf0) == 240) return true;
    return false;
}

bool v6_blocked(const unsigned char b[16]) noexcept {
    // ::/128 (unspecified) and ::1/128 (loopback).
    bool all_zero_but_last = true;
    for (int i = 0; i < 15; ++i)
        if (b[i] != 0) { all_zero_but_last = false; break; }
    if (all_zero_but_last && (b[15] == 0 || b[15] == 1)) return true;
    // v4-mapped (::ffff:a.b.c.d) and v4-compatible: apply the v4 rules, or
    // ::ffff:127.0.0.1 walks straight through an IPv6-only check.
    bool v4_mapped = true;
    for (int i = 0; i < 10; ++i)
        if (b[i] != 0) { v4_mapped = false; break; }
    if (v4_mapped && ((b[10] == 0xff && b[11] == 0xff) || (b[10] == 0 && b[11] == 0))) {
        unsigned char v4[4] = {b[12], b[13], b[14], b[15]};
        return v4_blocked(v4);
    }
    // fc00::/7 — unique local, the IPv6 LAN.
    if ((b[0] & 0xfe) == 0xfc) return true;
    // fe80::/10 — link-local.
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;
    // ff00::/8 — multicast.
    if (b[0] == 0xff) return true;
    // 2002::/16 (6to4) and 2001::/32 (Teredo) tunnel to v4 addresses we
    // cannot see from here, so they can carry a connection to the LAN.
    if (b[0] == 0x20 && b[1] == 0x02) return true;
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] == 0) return true;
    return false;
}

}  // namespace

bool is_blocked_address(std::string_view addr) noexcept {
#if defined(__linux__)
    // A string_view is not nul-terminated; inet_pton needs a C string.
    char buf[64];
    if (addr.empty() || addr.size() >= sizeof buf) return true;
    std::memcpy(buf, addr.data(), addr.size());
    buf[addr.size()] = '\0';

    unsigned char v6[16];
    if (::inet_pton(AF_INET6, buf, v6) == 1) return v6_blocked(v6);
    unsigned char v4[4];
    if (::inet_pton(AF_INET, buf, v4) == 1) return v4_blocked(v4);
    // Not an address at all. Refuse: every caller here has already resolved,
    // so an unparseable string is a bug, and a bug must fail closed.
    return true;
#else
    (void)addr;
    return true;
#endif
}

}  // namespace agentty::tools::util::sandbox::netproxy

// ── the parsers ─────────────────────────────────────────────────────────
//
// Outside the platform #if: these touch no descriptors and no platform
// headers, so they build (and are fuzzed) everywhere. Every length is checked
// against the buffer before it is used -- the guest chooses these bytes.
namespace agentty::tools::util::sandbox::netproxy {

namespace parse_detail {

// A hostname's hard limit (RFC 1035: 255 octets including the length bytes).
// Enforced in the parsers rather than left to the resolver: a 4 KB "host"
// is not a destination, and refusing it here keeps a silly string out of
// getaddrinfo and out of the log line that reports the refusal.
inline constexpr std::size_t kMaxHostLen = 255;

// A 16-bit big-endian read. The caller has already bounds-checked `at`.
[[nodiscard]] inline std::uint16_t be16(std::string_view s, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<unsigned char>(s[at]) << 8) | static_cast<unsigned char>(s[at + 1]));
}

// An address literal from its wire bytes, in presentation form. Hand-rolled
// rather than inet_ntop so the parsers stay free of platform headers.
[[nodiscard]] inline std::string v4_text(std::string_view b, std::size_t at) {
    std::string out;
    for (int i = 0; i < 4; ++i) {
        if (i) out += '.';
        out += std::to_string(static_cast<unsigned char>(b[at + std::size_t(i)]));
    }
    return out;
}

[[nodiscard]] inline std::string v6_text(std::string_view b, std::size_t at) {
    // The full, uncompressed form. getaddrinfo accepts it, and "correct but
    // verbose" beats a hand-written :: compressor with an off-by-one in it.
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (int g = 0; g < 8; ++g) {
        if (g) out += ':';
        const auto hi = static_cast<unsigned char>(b[at + std::size_t(g) * 2]);
        const auto lo = static_cast<unsigned char>(b[at + std::size_t(g) * 2 + 1]);
        out += kHex[hi >> 4];
        out += kHex[hi & 0xf];
        out += kHex[lo >> 4];
        out += kHex[lo & 0xf];
    }
    return out;
}

// A port from decimal digits, refusing anything that is not exactly a port.
// strtoul would take "80abc", "+80", " 80", and wrap on overflow.
[[nodiscard]] inline bool parse_port(std::string_view s, std::uint16_t& out) noexcept {
    if (s.empty() || s.size() > 5) return false;
    std::uint32_t v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<std::uint32_t>(ch - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;   // port 0 is not a destination
    out = static_cast<std::uint16_t>(v);
    return true;
}

}  // namespace parse_detail

Socks5Greeting parse_socks5_greeting(std::string_view in) noexcept {
    Socks5Greeting g;
    if (in.size() < 2) { g.need_more = true; return g; }
    if (static_cast<unsigned char>(in[0]) != 0x05) return g;   // not SOCKS5
    const std::size_t n = static_cast<unsigned char>(in[1]);
    if (in.size() < 2 + n) { g.need_more = true; return g; }
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<unsigned char>(in[2 + i]) == 0x00) g.no_auth = true;
    g.consumed = 2 + n;
    g.ok = true;
    return g;
}

Socks5Request parse_socks5_request(std::string_view in) {
    using namespace parse_detail;
    Socks5Request r;
    // 05 CMD 00 ATYP + at least one address byte + 2 port bytes
    if (in.size() < 7) { r.target.need_more = true; return r; }
    if (static_cast<unsigned char>(in[0]) != 0x05) return r;
    r.cmd  = static_cast<std::uint8_t>(in[1]);
    r.atyp = static_cast<std::uint8_t>(in[3]);

    std::size_t port_at = 0;
    if (r.atyp == 0x01) {            // IPv4: 4 bytes
        if (in.size() < 4 + 4 + 2) { r.target.need_more = true; return r; }
        r.target.host = v4_text(in, 4);
        port_at = 8;
    } else if (r.atyp == 0x03) {     // name: a length byte, then that many
        const std::size_t len = static_cast<unsigned char>(in[4]);
        if (in.size() < 5 + len + 2) { r.target.need_more = true; return r; }
        if (len == 0) return r;      // an empty host is not a destination
        r.target.host.assign(in.substr(5, len));
        // A "name" with a NUL or a slash in it is not a name. Refuse rather
        // than hand it to the resolver.
        if (r.target.host.find('\0') != std::string::npos
            || r.target.host.find('/') != std::string::npos)
            return r;
        // SOCKS5 caps a name at 255 by its length byte, so this is belt and
        // braces -- but the HTTP side has no such cap, and both go to the
        // same resolver.
        if (r.target.host.size() > kMaxHostLen) return r;
        port_at = 5 + len;
    } else if (r.atyp == 0x04) {     // IPv6: 16 bytes
        if (in.size() < 4 + 16 + 2) { r.target.need_more = true; return r; }
        r.target.host = v6_text(in, 4);
        port_at = 20;
    } else {
        return r;                    // unknown address type
    }

    r.target.port = be16(in, port_at);
    if (r.target.port == 0) return r;
    r.target.ok = true;
    return r;
}

HttpRequest parse_http_request(std::string_view head) {
    using namespace parse_detail;
    HttpRequest h;
    // "METHOD SP TARGET SP VERSION"; only the first two fields matter here.
    const auto sp1 = head.find(' ');
    if (sp1 == std::string_view::npos) {
        h.target.need_more = head.size() < 8192;
        return h;
    }
    const std::string_view method = head.substr(0, sp1);
    const auto sp2 = head.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        h.target.need_more = head.size() < 8192;
        return h;
    }
    const std::string_view target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    if (method.empty() || target.empty()) return h;

    std::string_view authority;
    if (method == "CONNECT") {
        h.connect = true;
        authority = target;          // authority-form: host:port, port required
    } else {
        // absolute-form: scheme://host[:port]/path
        const auto scheme = target.find("://");
        if (scheme == std::string_view::npos) return h;
        const auto start = scheme + 3;
        if (start >= target.size()) return h;
        const auto slash = target.find('/', start);
        authority = target.substr(start, slash == std::string_view::npos
                                             ? std::string_view::npos
                                             : slash - start);
        // userinfo@host is legal; the host is the part that matters.
        if (const auto at = authority.rfind('@'); at != std::string_view::npos)
            authority = authority.substr(at + 1);
    }
    if (authority.empty()) return h;

    if (authority.front() == '[') {
        // a bracketed IPv6 literal, with any port outside the brackets
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return h;
        h.target.host.assign(authority.substr(1, close - 1));
        const auto rest = authority.substr(close + 1);
        if (rest.empty()) {
            if (h.connect) return h;   // CONNECT needs an explicit port
            h.target.port = 80;
        } else if (rest.front() == ':') {
            if (!parse_port(rest.substr(1), h.target.port)) return h;
        } else {
            return h;
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            if (h.connect) return h;
            h.target.host.assign(authority);
            h.target.port = 80;
        } else {
            h.target.host.assign(authority.substr(0, colon));
            if (!parse_port(authority.substr(colon + 1), h.target.port)) return h;
        }
    }
    if (h.target.host.empty() || h.target.host.find('\0') != std::string::npos)
        return h;
    if (h.target.host.size() > parse_detail::kMaxHostLen) return h;
    h.target.ok = true;
    return h;
}

}  // namespace agentty::tools::util::sandbox::netproxy

#if !defined(__linux__) || defined(AGENTTY_NETPROXY_PARSERS_ONLY)

// The parsers above are the whole file when AGENTTY_NETPROXY_PARSERS_ONLY is
// set (the fuzz harness) or when this is not Linux. A Proxy still has to
// LINK, so it exists and refuses to start.
namespace agentty::tools::util::sandbox::netproxy {
Proxy::~Proxy() = default;
bool Proxy::start(int) { return false; }
void Proxy::stop(const maya::unlocked&) noexcept {}
bool Proxy::running() const noexcept { return false; }
std::uint64_t Proxy::refused() const noexcept { return 0; }
std::uint64_t Proxy::served() const noexcept { return 0; }
}  // namespace agentty::tools::util::sandbox::netproxy

#else

namespace agentty::tools::util::sandbox::netproxy {

namespace {

// Resolve `host` and return the first address that passes the filter, as a
// connected socket. -1 on refusal or failure; `refused` says which.
//
// The dial uses the SAME sockaddr the filter checked, so there is no window
// in which a second lookup could return a different answer.
int dial_filtered(const std::string& host, std::uint16_t port, bool& refused) {
    refused = false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string svc = std::to_string(port);
    if (::getaddrinfo(host.c_str(), svc.c_str(), &hints, &res) != 0 || !res)
        return -1;

    int fd = -1;
    bool any_blocked = false;
    for (addrinfo* a = res; a; a = a->ai_next) {
        char txt[64]{};
        const void* src = nullptr;
        if (a->ai_family == AF_INET)
            src = &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr;
        else if (a->ai_family == AF_INET6)
            src = &reinterpret_cast<sockaddr_in6*>(a->ai_addr)->sin6_addr;
        else
            continue;
        if (!::inet_ntop(a->ai_family, src, txt, sizeof txt)) continue;
        if (is_blocked_address(txt)) {
            any_blocked = true;
            AGT_LOG(Tool, Warn, "sandbox.proxy.refused", "host={} addr={} port={}",
                    host, txt, port);
            continue;
        }
        const int s = ::socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) continue;
        if (::connect(s, a->ai_addr, a->ai_addrlen) == 0) { fd = s; break; }
        ::close(s);
    }
    ::freeaddrinfo(res);
    if (fd < 0 && any_blocked) refused = true;
    return fd;
}

// Copy bytes both ways until either side closes. One poll loop, no threads:
// a connection is two descriptors and some buffering, not a subsystem.
void pump(int a, int b, const std::stop_token& stop) {
    char buf[32 * 1024];
    bool a_open = true, b_open = true;
    while ((a_open || b_open) && !stop.stop_requested()) {
        pollfd fds[2]{{a, POLLIN, 0}, {b, POLLIN, 0}};
        const int n = ::poll(fds, 2, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) continue;
        auto move = [&](int from, int to, bool& open) {
            const ssize_t got = ::read(from, buf, sizeof buf);
            if (got <= 0) {
                open = false;
                ::shutdown(to, SHUT_WR);
                return;
            }
            ssize_t sent = 0;
            while (sent < got) {
                const ssize_t w = ::write(to, buf + sent, static_cast<std::size_t>(got - sent));
                if (w <= 0) {
                    if (w < 0 && errno == EINTR) continue;
                    open = false;
                    return;
                }
                sent += w;
            }
        };
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) move(a, b, a_open);
        if (b_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) move(b, a, b_open);
    }
}

bool read_some(int fd, std::string& into, std::size_t cap, const std::stop_token& stop) {
    char buf[1024];
    while (into.size() < cap && !stop.stop_requested()) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 5000);
        if (r <= 0) return false;
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) return false;
        into.append(buf, static_cast<std::size_t>(n));
        // HTTP: a blank line ends the request. SOCKS: the caller checks its
        // own length, so returning early with "enough" is fine.
        if (into.find("\r\n\r\n") != std::string::npos) return true;
        if (into.size() >= 2 && static_cast<unsigned char>(into[0]) == 0x05) return true;
    }
    return !into.empty();
}

void write_all(int fd, std::string_view s) {
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t w = ::write(fd, s.data() + off, s.size() - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return;
        }
        off += static_cast<std::size_t>(w);
    }
}

// SOCKS5 (RFC 1928), the no-auth subset every client uses.
//
// The byte parsing lives in parse_socks5_* above, which is where the fuzzer
// reaches it; this is the socket half: read until the parser says it has
// enough, then answer.
int serve_socks5(int c, const std::string& first, std::atomic<std::uint64_t>& refused_count,
                 const std::stop_token& stop) {
    std::string in = first;
    Socks5Greeting g = parse_socks5_greeting(in);
    while (g.need_more) {
        if (!read_some(c, in, 512, stop)) return -1;
        g = parse_socks5_greeting(in);
    }
    if (!g.ok || !g.no_auth) {
        const char none[2] = {'\x05', '\xff'};   // no acceptable method
        write_all(c, std::string_view{none, sizeof none});
        return -1;
    }
    {
        const char ok[2] = {'\x05', '\x00'};
        write_all(c, std::string_view{ok, sizeof ok});
    }

    // The request may already be in the buffer behind the greeting (clients
    // pipeline it), so start from what is left rather than reading blind.
    std::string req = in.substr(g.consumed);
    Socks5Request r = parse_socks5_request(req);
    while (r.target.need_more) {
        if (!read_some(c, req, 512, stop)) return -1;
        r = parse_socks5_request(req);
    }
    if (!r.target.ok) {
        // 0x08 = address type not supported, which covers the malformed cases
        // closely enough for a client to report something useful.
        const char nosup[10] = {'\x05', '\x08', '\x00', '\x01', 0, 0, 0, 0, 0, 0};
        write_all(c, std::string_view{nosup, sizeof nosup});
        return -1;
    }
    // CONNECT only. BIND and UDP ASSOCIATE are both inbound paths, and
    // nothing a build does needs them.
    if (r.cmd != 0x01) {
        const char nocmd[10] = {'\x05', '\x07', '\x00', '\x01', 0, 0, 0, 0, 0, 0};
        write_all(c, std::string_view{nocmd, sizeof nocmd});
        return -1;
    }

    bool refused = false;
    const int up = dial_filtered(r.target.host, r.target.port, refused);
    if (up < 0) {
        if (refused) refused_count.fetch_add(1, std::memory_order_relaxed);
        // 0x02 = "connection not allowed by ruleset" for a refusal, which is
        // the honest code; 0x05 = connection refused otherwise.
        //
        // Built as an explicit 10-byte array, NOT a string literal: a SOCKS
        // reply is mostly NUL bytes, so a std::string built from one stops at
        // the first and the client sees a truncated reply (curl reported
        // "proxy closed connection" rather than the refusal it was sent).
        const char reply[10] = {'\x05', refused ? '\x02' : '\x05', '\x00', '\x01',
                                0, 0, 0, 0, 0, 0};
        write_all(c, std::string_view{reply, sizeof reply});
        return -1;
    }
    {
        const char ok[10] = {'\x05', '\x00', '\x00', '\x01', 0, 0, 0, 0, 0, 0};
        write_all(c, std::string_view{ok, sizeof ok});
    }
    return up;
}

// HTTP CONNECT, plus absolute-form requests for plain http:// URLs.
int serve_http(int c, const std::string& head, std::atomic<std::uint64_t>& refused_count,
               bool& forward_head, std::string& rest) {
    const HttpRequest h = parse_http_request(head);
    forward_head = h.target.ok && !h.connect;
    if (forward_head) rest = head;
    if (!h.target.ok) {
        write_all(c, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
        return -1;
    }

    bool refused = false;
    const int up = dial_filtered(h.target.host, h.target.port, refused);
    if (up < 0) {
        if (refused) refused_count.fetch_add(1, std::memory_order_relaxed);
        write_all(c, refused ? "HTTP/1.1 403 Forbidden\r\n"
                               "Content-Length: 0\r\n"
                               "X-Agentty-Sandbox: address refused by the sandbox proxy\r\n\r\n"
                             : "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
        return -1;
    }
    if (h.connect) write_all(c, "HTTP/1.1 200 Connection established\r\n\r\n");
    return up;
}

// Shared between the accept loop, the per-connection jobs and the owner, so
// every field is atomic: co_owned<T> requires Sync, which is the type system
// asking "is this safe from all of them at once" rather than taking a promise.
struct ProxyState {
    std::atomic<int> listen_fd{-1};
    std::atomic<bool> live{false};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> served{0};
};

}  // namespace

struct Proxy::Impl {
    ::maya::co_owned<ProxyState> st = ::maya::co_owned<ProxyState>::make();
    ::agentty::util::WorkerGroup group{"sandbox.netproxy"};
};

Proxy::~Proxy() {
    // The group joins in its own destructor, which is the barrier that makes
    // the connection jobs' use of ProxyState safe. stop(cap) is the explicit
    // path; this is the backstop for an owner that forgot.
    if (impl_) {
        const int lfd = impl_->st->listen_fd.exchange(-1, std::memory_order_relaxed);
        if (lfd >= 0) {
            ::shutdown(lfd, SHUT_RDWR);
            ::close(lfd);
        }
    }
    delete impl_;
    impl_ = nullptr;
}

bool Proxy::start(int listen_fd) {
    if (listen_fd < 0) return false;
    if (impl_) { ::close(listen_fd); return false; }
    impl_ = new Impl{};
    impl_->st->listen_fd.store(listen_fd, std::memory_order_relaxed);
    impl_->st->live.store(true, std::memory_order_relaxed);

    // ONE job: accept, then serve. Serving inline keeps the whole proxy to a
    // single background job, and a sandboxed build's traffic is a handful of
    // connections, not a web server's. The cost is that a slow upstream
    // delays the NEXT accept, which is a queue rather than a failure.
    impl_->group.post(
        [](const maya::unlocked&, std::stop_token stop, ::maya::co_owned<ProxyState> st) {
            while (!stop.stop_requested()) {
                const int lfd = st->listen_fd.load(std::memory_order_relaxed);
                if (lfd < 0) break;
                pollfd p{lfd, POLLIN, 0};
                const int r = ::poll(&p, 1, 300);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (r == 0) continue;
                if (!(p.revents & POLLIN)) {
                    if (p.revents & (POLLERR | POLLNVAL)) break;
                    continue;
                }
                const int c = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
                if (c < 0) {
                    if (errno == EINTR || errno == EAGAIN) continue;
                    break;
                }
                st->served.fetch_add(1, std::memory_order_relaxed);
                std::string first;
                if (!read_some(c, first, 8192, stop) || first.empty()) {
                    ::close(c);
                    continue;
                }
                int up = -1;
                std::string replay;
                if (static_cast<unsigned char>(first[0]) == 0x05) {
                    up = serve_socks5(c, first, st->refused, stop);
                } else {
                    bool forward_head = false;
                    up = serve_http(c, first, st->refused, forward_head, replay);
                    if (up >= 0 && forward_head) write_all(up, replay);
                }
                if (up < 0) {
                    ::close(c);
                    continue;
                }
                pump(c, up, stop);
                ::close(up);
                ::close(c);
            }
            st->live.store(false, std::memory_order_relaxed);
        },
        impl_->st);
    return true;
}

void Proxy::stop(const maya::unlocked& cap) noexcept {
    if (!impl_) return;
    // Closing the listener wakes the accept poll immediately, rather than
    // waiting out its 300 ms tick.
    const int lfd = impl_->st->listen_fd.exchange(-1, std::memory_order_relaxed);
    if (lfd >= 0) {
        ::shutdown(lfd, SHUT_RDWR);
        ::close(lfd);
    }
    impl_->group.stop(cap);
    impl_->st->live.store(false, std::memory_order_relaxed);
}

bool Proxy::running() const noexcept {
    return impl_ && impl_->st->live.load(std::memory_order_relaxed);
}
std::uint64_t Proxy::refused() const noexcept {
    return impl_ ? impl_->st->refused.load(std::memory_order_relaxed) : 0;
}
std::uint64_t Proxy::served() const noexcept {
    return impl_ ? impl_->st->served.load(std::memory_order_relaxed) : 0;
}

}  // namespace agentty::tools::util::sandbox::netproxy

#endif  // __linux__ && !AGENTTY_NETPROXY_PARSERS_ONLY
