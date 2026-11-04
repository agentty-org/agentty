// The sandbox's outbound network, filtered — the host side of claybin's
// proxied network namespace.
//
// WHY THIS EXISTS. "The sandbox may use the network" used to mean the box
// shared agentty's network namespace, and that is not a boundary: a command
// the agent runs could reach every service on the host's loopback (a dev
// server, a database, an unauthenticated admin port, another agent's debug
// socket) and every machine on the LAN (the router's admin page, a NAS, a
// colleague's laptop). The sandbox audit called this its one High finding,
// and it was right: the filesystem walls were solid and the network had none.
//
// Landlock can filter TCP connects in the kernel, which is stronger than any
// proxy — but only by PORT. ":443 to the package registry" and ":443 to the
// router" are one rule, so port rules cannot express the thing that matters.
//
// So: claybin puts the guest in its OWN network namespace (nothing outside is
// reachable at all, enforced by the kernel) and opens one listener on the
// guest's loopback, handing the descriptor out. This file accepts on that
// descriptor and speaks two proxy protocols the whole toolchain already
// knows — HTTP CONNECT and SOCKS5 — resolving each requested name HERE and
// refusing the addresses that were the actual hole.
//
// WHAT IT REFUSES, and why each one:
//   127.0.0.0/8, ::1     the host's own services. the finding.
//   10/8, 172.16/12, 192.168/16, fc00::/7   the LAN. the finding.
//   169.254/16, fe80::/10  link-local, which includes cloud metadata
//                          (169.254.169.254 hands out IAM credentials).
//   100.64/10            CGNAT, which is Tailscale's range — a tailnet is
//                        someone's private network by another name.
//   0.0.0.0/8, 224/4, 240/4, multicast  not destinations.
//
// The filter runs AFTER resolution and on the address actually dialled, so a
// name that resolves to 127.0.0.1 is refused on the address, not the spelling.
// There is no rebinding window: the proxy dials the address it checked.
//
// HONEST LIMITS. This is a policy wall, not a kernel one: it decides what the
// proxy will dial, so its strength is this code being correct. What the KERNEL
// guarantees is the part that matters most — without a descriptor from here
// the guest has no route off its own loopback, so a bug in this file fails
// closed (connection refused) rather than open.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <maya/runtime.hpp>   // maya::unlocked

namespace agentty::tools::util::sandbox::netproxy {

// Is this address one the guest must not reach? `addr` is a resolved address
// in presentation form (inet_ntop), not a name.
//
// Pure and exported for the test: every entry in the list above is pinned.
[[nodiscard]] bool is_blocked_address(std::string_view addr) noexcept;

// Is `host` (as the guest asked for it, before resolution) on the user's
// allowlist? An EMPTY list allows every name -- the address filter above
// still applies -- so this only ever narrows.
//
// Entries: `example.com` matches exactly that name; `*.example.com` matches
// any name strictly under it, but NOT the apex (say both if you mean both:
// the common slip is allowing `*.github.com` and wondering why github.com
// itself fails, and the opposite slip silently grants every subdomain).
// Case-insensitive, a trailing dot is ignored. An IP-literal host is refused
// whenever a list is set: an allowlist of names cannot vouch for an address,
// and allowing literals would make every name rule bypassable.
//
// Checked on the NAME, not the address: the guest cannot steer it with DNS,
// because the proxy resolves the very name it checked.
[[nodiscard]] bool host_allowed(std::string_view host,
                                const std::vector<std::string>& allow) noexcept;

// Canonicalise one allowlist entry as a user typed it, or say why it is not
// one. The settings editor and the proxy both go through this, so a list the
// pane accepted is exactly the list that is enforced.
//
// Forgiving where intent is unambiguous -- people paste URLs:
//   "https://GitHub.com/foo"  -> "github.com"      (scheme, path, case)
//   "github.com:443"          -> "github.com"      (port: the list is names)
//   ".github.com"             -> "*.github.com"    (curl/no_proxy spelling)
//   "github.com."             -> "github.com"
// Strict where it is not: an IP literal, a bare "*", a wildcard anywhere but
// the leftmost label, "*.com" (a whole TLD), or a character no host name can
// contain are REJECTED with a reason, never guessed at.
struct HostEntry {
    std::string value;    // canonical form; empty when rejected
    std::string_view why; // rejection reason (static text), empty when accepted
};
[[nodiscard]] HostEntry normalize_host_entry(std::string_view raw);

// ── the parsers, as pure functions ──────────────────────────────────────
//
// Split out from the socket code because they read bytes the GUEST chose:
// they are the one attacker-controlled parser in the sandbox's host side, so
// they are fuzzed (tests/fuzz/fuzz_netproxy.cpp) rather than trusted. No
// descriptors, no allocation beyond the output strings, no I/O.

struct Target {
    std::string host;        // a name or a literal, exactly as asked for
    std::uint16_t port{0};
    bool ok{false};          // false = malformed, and `need_more` says which
    bool need_more{false};   // the request is a valid PREFIX; read and retry
};

// SOCKS5 greeting (RFC 1928 §3): 05 NMETHODS METHODS...
// `ok` means a complete greeting that offers no-auth.
struct Socks5Greeting {
    bool ok{false};
    bool need_more{false};
    bool no_auth{false};
    std::size_t consumed{0};
};
[[nodiscard]] Socks5Greeting parse_socks5_greeting(std::string_view in) noexcept;

// SOCKS5 request (§4): 05 CMD 00 ATYP ADDR PORT. `cmd` is reported so the
// caller can refuse anything but CONNECT with the right reply code.
// `consumed` is how many bytes the request occupied: what follows is the
// client's own data, and forwarding the request bytes with it corrupts the
// stream (curl reported a TLS version error).
struct Socks5Request {
    Target target;
    std::uint8_t cmd{0};
    std::uint8_t atyp{0};
    std::size_t consumed{0};
};
[[nodiscard]] Socks5Request parse_socks5_request(std::string_view in);

// An HTTP request line: CONNECT host:port, or an absolute-form GET/POST/...
// `connect` distinguishes the two, because one gets a 200 and a tunnel and
// the other gets its head forwarded upstream.
struct HttpRequest {
    Target target;
    bool connect{false};
};
[[nodiscard]] HttpRequest parse_http_request(std::string_view head);

// A running proxy: owns the accept loop and its worker, joins on destruction.
//
// Construct it with the listener claybin handed back (it takes ownership) and
// it serves connections until stopped. `port` is where the listener is bound
// INSIDE the guest's namespace, which is what the guest's proxy env vars name.
class Proxy {
  public:
    Proxy() = default;
    ~Proxy();
    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;

    // Takes ownership of `listen_fd`. Returns false (and closes it) when the
    // worker could not start. `allow_hosts` is the name allowlist, fixed for
    // the proxy's lifetime (see host_allowed); empty = any public host.
    bool start(int listen_fd, std::vector<std::string> allow_hosts = {});

    // Stop accepting and join. Idempotent.
    //
    // Takes `unlocked` because joining can wait on any lock a connection job
    // needs: stopping from inside a lock body must not compile. The
    // destructor cannot ask for one, so the OWNER has to stop it -- which is
    // the same contract WorkerGroup itself has.
    void stop(const maya::unlocked& cap) noexcept;

    [[nodiscard]] bool running() const noexcept;

    // Connections refused by the address filter, for the audit trail.
    [[nodiscard]] std::uint64_t refused() const noexcept;
    [[nodiscard]] std::uint64_t served() const noexcept;

  private:
    struct Impl;
    Impl* impl_{nullptr};
};

}  // namespace agentty::tools::util::sandbox::netproxy
