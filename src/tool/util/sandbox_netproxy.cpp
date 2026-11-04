// The sandbox's filtered outbound network. See the header for why.
#include "agentty/tool/util/sandbox_netproxy.hpp"

#include "agentty/util/background.hpp"
#include "agentty/util/logx.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
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
    r.consumed = port_at + 2;
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

// Shared between the accept loop and the owner, so every field is atomic:
// co_owned<T> requires Sync, which is the type system asking "is this safe
// from all of them at once" rather than taking a promise.
struct ProxyState {
    std::atomic<int> listen_fd{-1};
    std::atomic<bool> live{false};
    std::atomic<std::uint64_t> refused{0};
    std::atomic<std::uint64_t> served{0};
};

// ── one connection, as a state machine ──────────────────────────────────
//
// WHY NOT A THREAD EACH, or a blocking handler per connection. The first
// version read, dialled and pumped inline, which meant the accept loop was
// busy for the whole life of one connection: `npm install` opens a dozen
// sockets at once and they queued behind each other. A thread each would fix
// that and cost a thread per fetch, plus a stop/join path for every one.
//
// So each connection is a small state machine, and ONE poll drives all of
// them plus the listener. The only blocking call left in the proxy is that
// poll. `dial_filtered` is the exception and it is deliberate: it resolves a
// name, which means it blocks, and a non-blocking resolver is a much bigger
// thing than this file should contain. One connection's DNS lookup can stall
// the others; its data never does.
struct Conn {
    enum class Phase : std::uint8_t {
        greet,      // SOCKS5: reading the method list
        request,    // reading the SOCKS5 request / the HTTP head
        relay,      // both sides open, moving bytes
        done,
    };

    int client{-1};
    int up{-1};
    Phase phase{Phase::greet};
    bool socks{false};        // decided by the first byte
    bool decided{false};      // have we seen that byte yet
    bool http_connect{false}; // CONNECT (a tunnel) vs absolute-form (forwarded)
    std::string in;           // what the client has sent and we have not parsed
    std::string to_up;        // bytes queued for the upstream (the HTTP head)
    bool client_open{true};
    bool up_open{true};

    void close_all() {
        if (up >= 0) { ::close(up); up = -1; }
        if (client >= 0) { ::close(client); client = -1; }
        phase = Phase::done;
    }
};

// Read whatever is available, without blocking. false = the client is gone.
bool conn_fill(Conn& c) {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(c.client, buf, sizeof buf);
        if (n > 0) {
            if (c.in.size() + static_cast<std::size_t>(n) > 64 * 1024) return false;
            c.in.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) return false;                     // orderly close
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
}

// Non-blocking write of everything we can; what is left stays queued.
void conn_flush(int fd, std::string& out, bool& open) {
    while (!out.empty()) {
        const ssize_t w = ::write(fd, out.data(), out.size());
        if (w > 0) {
            out.erase(0, static_cast<std::size_t>(w));
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        open = false;
        return;
    }
}

void set_nonblocking(int fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Advance one connection as far as the bytes it has allow.
void conn_step(Conn& c, std::atomic<std::uint64_t>& refused_count) {
    if (c.phase == Conn::Phase::done) return;

    if (!c.decided) {
        if (c.in.empty()) return;
        c.socks = static_cast<unsigned char>(c.in[0]) == 0x05;
        c.decided = true;
        if (!c.socks) c.phase = Conn::Phase::request;   // HTTP has no greeting
    }

    if (c.phase == Conn::Phase::greet) {
        const auto g = parse_socks5_greeting(c.in);
        if (g.need_more) return;
        if (!g.ok || !g.no_auth) {
            const char none[2] = {'\x05', '\xff'};   // no acceptable method
            std::string reply{none, sizeof none};
            bool open = true;
            conn_flush(c.client, reply, open);
            c.close_all();
            return;
        }
        {
            const char ok[2] = {'\x05', '\x00'};
            std::string reply{ok, sizeof ok};
            bool open = true;
            conn_flush(c.client, reply, open);
        }
        c.in.erase(0, g.consumed);
        c.phase = Conn::Phase::request;
    }

    if (c.phase == Conn::Phase::request) {
        std::string host;
        std::uint16_t port = 0;
        if (c.socks) {
            const auto r = parse_socks5_request(c.in);
            if (r.target.need_more) return;
            auto fail = [&](char code) {
                const char rep[10] = {'\x05', code, '\x00', '\x01', 0, 0, 0, 0, 0, 0};
                std::string reply{rep, sizeof rep};
                bool open = true;
                conn_flush(c.client, reply, open);
                c.close_all();
            };
            if (!r.target.ok) { fail('\x08'); return; }   // address type / malformed
            if (r.cmd != 0x01) { fail('\x07'); return; }  // CONNECT only
            host = r.target.host;
            port = r.target.port;
            // Drop the request itself: everything after it is the client's
            // own payload, and relaying the request bytes upstream corrupts
            // the stream (TLS fails with a version error).
            c.in.erase(0, r.consumed);
        } else {
            const auto h = parse_http_request(c.in);
            if (h.target.need_more) return;
            if (!h.target.ok) {
                std::string reply = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                bool open = true;
                conn_flush(c.client, reply, open);
                c.close_all();
                return;
            }
            host = h.target.host;
            port = h.target.port;
            c.http_connect = h.connect;
            // An absolute-form request is forwarded head and all; CONNECT is
            // a tunnel, so the head stops here.
            if (!h.connect) c.to_up = c.in;
            c.in.clear();
        }

        bool refused = false;
        c.up = dial_filtered(host, port, refused);
        if (c.up < 0) {
            if (refused) refused_count.fetch_add(1, std::memory_order_relaxed);
            std::string reply;
            if (c.socks) {
                // 0x02 = not allowed by ruleset, which is the honest code for
                // a refusal; 0x05 = connection refused otherwise.
                const char rep[10] = {'\x05', refused ? '\x02' : '\x05', '\x00', '\x01',
                                      0, 0, 0, 0, 0, 0};
                reply.assign(rep, sizeof rep);
            } else {
                reply = refused
                    ? "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                      "X-Agentty-Sandbox: address refused by the sandbox proxy\r\n\r\n"
                    : "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            }
            bool open = true;
            conn_flush(c.client, reply, open);
            c.close_all();
            return;
        }
        set_nonblocking(c.up);

        std::string ack;
        if (c.socks) {
            const char ok[10] = {'\x05', '\x00', '\x00', '\x01', 0, 0, 0, 0, 0, 0};
            ack.assign(ok, sizeof ok);
        } else if (c.http_connect) {
            ack = "HTTP/1.1 200 Connection established\r\n\r\n";
        }
        if (!ack.empty()) {
            bool open = true;
            conn_flush(c.client, ack, open);
        }
        // Anything the client pipelined behind the request belongs upstream.
        c.to_up += c.in;
        c.in.clear();
        c.phase = Conn::Phase::relay;
    }
}

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
    // The listener arrives BLOCKING: claybin's setup creates it without
    // SOCK_NONBLOCK, and the accept loop below drains it until EAGAIN. On a
    // blocking listener that second accept4 never returns, so the proxy
    // stalls after its first connection and every later request times out.
    // (Found exactly that way.)
    set_nonblocking(listen_fd);
    impl_->st->listen_fd.store(listen_fd, std::memory_order_relaxed);
    impl_->st->live.store(true, std::memory_order_relaxed);

    // ONE job, and ONE poll inside it, covering the listener and every live
    // connection. See the Conn comment: a blocking handler per connection
    // made `npm install` serialise its fetches behind each other.
    impl_->group.post(
        [](const maya::unlocked&, std::stop_token stop, ::maya::co_owned<ProxyState> st) {
            std::vector<Conn> conns;
            std::vector<pollfd> fds;
            while (!stop.stop_requested()) {
                const int lfd = st->listen_fd.load(std::memory_order_relaxed);
                if (lfd < 0) break;

                fds.clear();
                fds.push_back(pollfd{lfd, POLLIN, 0});
                for (auto& c : conns) {
                    if (c.phase == Conn::Phase::done) continue;
                    short cev = 0;
                    // Read from the client while we still want its bytes and
                    // are not sitting on a backlog for the upstream.
                    if (c.client_open && c.to_up.size() < 256 * 1024) cev |= POLLIN;
                    fds.push_back(pollfd{c.client, cev, 0});
                    if (c.up >= 0) {
                        short uev = 0;
                        if (c.up_open) uev |= POLLIN;
                        if (!c.to_up.empty()) uev |= POLLOUT;
                        fds.push_back(pollfd{c.up, uev, 0});
                    }
                }

                const int r = ::poll(fds.data(), fds.size(), 300);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }

                // The listener first, so a new connection does not wait a
                // tick behind the existing ones.
                if (fds[0].revents & (POLLERR | POLLNVAL)) break;
                if (fds[0].revents & POLLIN) {
                    for (;;) {
                        const int c = ::accept4(lfd, nullptr, nullptr,
                                                SOCK_CLOEXEC | SOCK_NONBLOCK);
                        if (c < 0) break;   // EAGAIN: drained
                        st->served.fetch_add(1, std::memory_order_relaxed);
                        if (conns.size() >= 256) {   // a cap, not a policy
                            ::close(c);
                            continue;
                        }
                        Conn nc;
                        nc.client = c;
                        conns.push_back(std::move(nc));
                    }
                }

                // Then every connection. The fd->revents mapping is rebuilt
                // each tick, so look each one up rather than remembering an
                // index across a vector that just grew.
                auto revents_of = [&](int fd) -> short {
                    for (const auto& p : fds)
                        if (p.fd == fd) return p.revents;
                    return 0;
                };
                for (auto& c : conns) {
                    if (c.phase == Conn::Phase::done) continue;
                    const short cr = revents_of(c.client);
                    const short ur = c.up >= 0 ? revents_of(c.up) : short{0};

                    if (cr & POLLIN) {
                        if (!conn_fill(c)) c.client_open = false;
                    }
                    if (c.phase != Conn::Phase::relay) {
                        conn_step(c, st->refused);
                        if (c.phase == Conn::Phase::done) continue;
                        // A client that hung up before finishing its request
                        // has nothing more to say.
                        if (!c.client_open && c.phase != Conn::Phase::relay) {
                            c.close_all();
                            continue;
                        }
                    }
                    if (c.phase != Conn::Phase::relay) continue;

                    // Relay. `in` holds client->upstream bytes, `to_up` is
                    // what has not been written yet; the reverse direction
                    // writes straight through because the client is the one
                    // that asked for the data.
                    if (!c.in.empty()) { c.to_up += c.in; c.in.clear(); }
                    if (ur & POLLOUT) conn_flush(c.up, c.to_up, c.up_open);
                    if (ur & POLLIN) {
                        char buf[16 * 1024];
                        for (;;) {
                            const ssize_t n = ::read(c.up, buf, sizeof buf);
                            if (n > 0) {
                                std::string out{buf, static_cast<std::size_t>(n)};
                                conn_flush(c.client, out, c.client_open);
                                if (!out.empty()) {
                                    // The client is slower than the upstream.
                                    // Blocking here is the honest thing: the
                                    // alternative is an unbounded buffer, and
                                    // this only stalls THIS connection.
                                    pollfd w{c.client, POLLOUT, 0};
                                    if (::poll(&w, 1, 2000) == 1)
                                        conn_flush(c.client, out, c.client_open);
                                    else
                                        c.client_open = false;
                                }
                                continue;
                            }
                            if (n == 0) { c.up_open = false; break; }
                            if (errno == EINTR) continue;
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            c.up_open = false;
                            break;
                        }
                    }
                    if ((cr | ur) & (POLLERR | POLLNVAL)) {
                        c.close_all();
                        continue;
                    }
                    // Hangup on either side with nothing left to move ends it.
                    const bool client_gone = !c.client_open || (cr & POLLHUP);
                    const bool up_gone = !c.up_open || (ur & POLLHUP);
                    if ((client_gone && c.to_up.empty()) || (up_gone && !c.client_open)
                        || (up_gone && client_gone))
                        c.close_all();
                    else if (client_gone && c.to_up.empty() && c.up >= 0)
                        ::shutdown(c.up, SHUT_WR);
                }

                std::erase_if(conns, [](const Conn& c) {
                    return c.phase == Conn::Phase::done;
                });
            }
            for (auto& c : conns) c.close_all();
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
