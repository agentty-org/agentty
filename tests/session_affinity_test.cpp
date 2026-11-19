// session_affinity_test — #118: a routing gateway (bifrost, OpenRouter) keeps
// a conversation on the backend that served it last when every request
// carries the same `x-session-id`. These tests stream a real turn through the
// OpenAI-compatible transport to a loopback server and read what arrived.

#include "agtest.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentty/provider/error_class.hpp"
#include "agentty/provider/openai/provider.hpp"
#include "agentty/provider/openai/transport.hpp"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

using namespace agentty;
namespace oai = agentty::provider::openai;

namespace {

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalid = INVALID_SOCKET;
void close_socket(socket_t s) { if (s != kInvalid) ::closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kInvalid = -1;
void close_socket(socket_t s) { if (s != kInvalid) ::close(s); }
#endif

// Answers each POST with a one-word SSE turn and keeps the request heads.
class ChatServer {
public:
    ChatServer() {
#if defined(_WIN32)
        WSADATA d{};
        if (::WSAStartup(MAKEWORD(2, 2), &d) != 0) return;
        wsa_ = true;
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == kInvalid) return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0
            || ::listen(fd_, 8) != 0) {
            close_socket(fd_);
            fd_ = kInvalid;
            return;
        }
#if defined(_WIN32)
        int len = sizeof(a);
#else
        socklen_t len = sizeof(a);
#endif
        (void)::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~ChatServer() {
        stop_.store(true);
        if (fd_ != kInvalid) {   // wake accept()
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(port_);
            socket_t w = ::socket(AF_INET, SOCK_STREAM, 0);
            if (w != kInvalid) {
                (void)::connect(w, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
                close_socket(w);
            }
        }
        if (worker_.joinable()) worker_.join();
        close_socket(fd_);
#if defined(_WIN32)
        if (wsa_) ::WSACleanup();
#endif
    }

    [[nodiscard]] bool valid() const { return fd_ != kInvalid; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

    // The value of `name` in the last request head, lowercased name match.
    [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
        std::lock_guard lk(mu_);
        std::size_t pos = 0;
        while (pos < last_head_.size()) {
            const auto eol = last_head_.find("\r\n", pos);
            const auto line = last_head_.substr(pos, eol == std::string::npos ? std::string::npos
                                                                              : eol - pos);
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (key == name) {
                    auto v = line.substr(colon + 1);
                    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                    return v;
                }
            }
            if (eol == std::string::npos) break;
            pos = eol + 2;
        }
        return std::nullopt;
    }
    [[nodiscard]] std::string body() const { std::lock_guard lk(mu_); return last_body_; }
    [[nodiscard]] int requests() const { return requests_.load(); }
    // Refuse (400, OpenCode Go's exact words) any request lacking `name`.
    void require_header(std::string name) { std::lock_guard lk(mu_); required_ = std::move(name); }

private:
    void serve() {
        while (!stop_.load()) {
            const socket_t c = ::accept(fd_, nullptr, nullptr);
            if (c == kInvalid) continue;
            if (stop_.load()) { close_socket(c); break; }
#if defined(_WIN32)
            const DWORD ms = 5000;
            (void)::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
                               reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
            timeval tv{5, 0};
            (void)::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            handle(c);
            close_socket(c);
        }
    }

    void handle(socket_t c) {
        std::string in;
        char buf[4096];
        std::size_t head_end = std::string::npos, want = 0;
        for (;;) {
            const auto n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) return;
            in.append(buf, static_cast<std::size_t>(n));
            if (head_end == std::string::npos && (head_end = in.find("\r\n\r\n")) != std::string::npos) {
                std::string lower = in.substr(0, head_end);
                for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (const auto m = lower.find("content-length:"); m != std::string::npos)
                    want = std::strtoull(lower.c_str() + m + 15, nullptr, 10);
            }
            if (head_end != std::string::npos && in.size() >= head_end + 4 + want) break;
        }
        std::string required;
        {
            std::lock_guard lk(mu_);
            last_head_ = in.substr(0, head_end);
            last_body_ = in.substr(head_end + 4, want);
            required = required_;
        }
        ++requests_;
        if (!required.empty() && !header(required)) {
            const std::string err =
                "{\"type\":\"error\",\"error\":{\"type\":\"BadRequestError\",\"message\":"
                "\"Request is missing " + required + " and cannot be routed efficiently.\"}}";
            const std::string r400 =
                "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\n"
                "Content-Length: " + std::to_string(err.size()) + "\r\n\r\n" + err;
            (void)::send(c, r400.data(), static_cast<int>(r400.size()), 0);
            return;
        }
        const std::string sse =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"}}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n";
        const std::string resp =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n"
            "Content-Length: " + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        std::size_t sent = 0;
        while (sent < resp.size()) {
            const auto n = ::send(c, resp.data() + sent, static_cast<int>(resp.size() - sent), 0);
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    socket_t fd_ = kInvalid;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    mutable std::mutex mu_;
    std::string last_head_, last_body_, required_;
    std::atomic<int> requests_{0};
#if defined(_WIN32)
    bool wsa_ = false;
#endif
};

// A custom host (no registry row): the shape a self-run bifrost has.
oai::Endpoint gateway(std::uint16_t port) {
    oai::Endpoint ep;
    ep.host    = "127.0.0.1";
    ep.port    = port;
    ep.path    = "/v1/chat/completions";
    ep.use_tls = false;
    ep.label   = "127.0.0.1:" + std::to_string(port);
    return ep;
}

provider::Request turn(std::string session_key, std::string affinity_key = {}) {
    provider::Request req;
    req.model        = "router/auto";
    req.max_tokens   = 64;
    req.session_key  = std::move(session_key);
    req.affinity_key = std::move(affinity_key);
    Message m;
    m.role = Role::User;
    m.text = "hi";
    req.messages.push_back(std::move(m));
    return req;
}

provider::StreamResult send(const ChatServer& s, provider::Request req) {
    oai::OpenAIProvider p{gateway(s.port())};
    return p.stream(agtest::cap(), std::move(req), [](Msg) {});
}

} // namespace

TEST_CASE("session affinity: the thread id rides x-session-id (#118)") {
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    const auto r = send(s, turn("thread-0123"));
    CHECK(r.ok());
    CHECK(s.header("x-session-id") == std::optional<std::string>{"thread-0123"});
}

TEST_CASE("session affinity: a subagent sticks with its parent's session") {
    // The subagent keeps its own prompt-cache key, but the gateway session
    // is the parent thread's, so it lands where the chat already is.
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    const auto r = send(s, turn("subagent:explorer:abcd", "parent-thread"));
    CHECK(r.ok());
    CHECK(s.header("x-session-id") == std::optional<std::string>{"parent-thread"});
}

TEST_CASE("session affinity: no conversation, no header") {
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    const auto r = send(s, turn(""));
    CHECK(r.ok());
    CHECK(!s.header("x-session-id").has_value());
}

TEST_CASE("session affinity: the header is all a custom host gets") {
    // prompt_cache_key stays off for hosts that didn't opt in (#93): the
    // session rides a header, which an unknown host ignores, never the body.
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    (void)send(s, turn("thread-0123"));
    CHECK(s.body().find("prompt_cache_key") == std::string::npos);
}

TEST_CASE("session affinity: provider::Request::affinity falls back to the session") {
    provider::Request req;
    CHECK(req.affinity().empty());
    req.session_key = "t";
    CHECK(req.affinity() == "t");
    req.affinity_key = "parent";
    CHECK(req.affinity() == "parent");
}

// ── Session header: learned from the host, for every provider ──────────────────

TEST_CASE("session header: a host that names the header it needs gets it, once, and keeps it") {
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    s.require_header("x-opencode-session");
    const std::string host = host_key("127.0.0.1", s.port());
    forget_session_header(host);

    const auto r = send(s, turn("thread-oc"));
    CHECK_MESSAGE(r.ok(), "the refused request was resent with the header and went through");
    CHECK(s.requests() == 2);
    CHECK(s.header("x-opencode-session") == std::optional<std::string>{"thread-oc"});
    CHECK_MESSAGE(session_header_entry(host) == std::optional<std::string>{"x-opencode-session"},
                  "learned for this host, so it can be persisted");

    // The next turn sends it up front: no second refusal.
    (void)send(s, turn("thread-oc"));
    CHECK(s.requests() == 3);
    forget_session_header(host);
}

TEST_CASE("session header: a host still refusing the learned header is not resent forever") {
    ChatServer s;
    REQUIRE(s.valid(), "loopback server starts");
    // Demands one header, but the user pinned another: no learning, one try.
    s.require_header("x-opencode-session");
    const std::string host = host_key("127.0.0.1", s.port());
    set_session_header_override(host, "x-my-session");
    const auto r = send(s, turn("thread-pin"));
    CHECK(!r.ok());
    CHECK(s.requests() == 1);
    CHECK(s.header("x-my-session") == std::optional<std::string>{"thread-pin"});
    REQUIRE(r.error.has_value());
    CHECK_MESSAGE(r.error->find("x-my-session") != std::string::npos,
                  "the error says what was sent instead of blaming the key");
    forget_session_header(host);
}

TEST_CASE("session header: what a server can make agentty learn is narrow") {
    using provider::parse_required_session_header;
    CHECK(parse_required_session_header(
              "Request is missing x-opencode-session and cannot be routed efficiently.", 400)
          == "x-opencode-session");
    CHECK(parse_required_session_header("Header X-Conversation-Id is required", 422)
          == "x-conversation-id");
    CHECK(parse_required_session_header("missing x-thread-key", 0) == "x-thread-key");
    // Not session-shaped, or not a header name: never learned.
    CHECK(parse_required_session_header("missing x-api-key", 401).empty());
    CHECK(parse_required_session_header("missing authorization (session expired)", 401).empty());
    CHECK(parse_required_session_header("missing cookie session", 400).empty());
    // Mentioned, but not missing: a value error isn't a missing header.
    CHECK(parse_required_session_header("x-session-id must be a uuid", 400).empty());
    // Server errors are weather, not contract.
    CHECK(parse_required_session_header("missing x-session-id", 503).empty());
    // Overlong names are refused.
    CHECK(parse_required_session_header(
              "missing x-" + std::string(80, 'a') + "-session", 400).empty());
}

TEST_CASE("session header: a hosted vendor gets an opaque per-vendor id, your own host the thread id") {
    namespace oai2 = agentty::provider::openai;
    const auto a = oai2::session_header_value("thread-1", "opencode.ai", "opencode-go");
    const auto b = oai2::session_header_value("thread-1", "openrouter.ai", "openrouter");
    CHECK(a.size() == 32);
    CHECK(a.find("thread-1") == std::string::npos);
    CHECK_MESSAGE(a != b, "two vendors can't join the same conversation by its id");
    CHECK_MESSAGE(a == oai2::session_header_value("thread-1", "opencode.ai", "opencode-go"),
                  "stable for the conversation (that's the point)");
    CHECK(a != oai2::session_header_value("thread-2", "opencode.ai", "opencode-go"));
    CHECK(oai2::session_header_value("thread-1", "127.0.0.1", "127.0.0.1:8080") == "thread-1");
}

TEST_CASE("session header: OpenCode Go's row sends x-opencode-session from the first request") {
    namespace oai2 = agentty::provider::openai;
    const auto ep = oai2::Endpoint::from_spec("opencode-go");
    CHECK(ep.host == "opencode.ai");
    CHECK(ep.path == "/inference/go/v1/chat/completions");
    forget_session_header(host_key(ep.host, ep.port));
    const auto c = oai2::session_header_for(ep.host, ep.port, ep.label);
    CHECK(c.name == "x-opencode-session");
    CHECK(c.source == oai2::SessionHeaderSource::Row);
    // Even with no conversation (a model-list probe) a requiring host gets one.
    http::Headers h;
    oai2::append_session_header(h, {}, ep.host, ep.port, ep.label);
    REQUIRE(h.size() == 1u);
    CHECK(h[0].name == "x-opencode-session");
    CHECK(!h[0].value.empty());
    // ...while a default host with no conversation gets nothing, as before.
    http::Headers none;
    oai2::append_session_header(none, {}, "127.0.0.1", 9, "127.0.0.1:9");
    CHECK(none.empty());
}
