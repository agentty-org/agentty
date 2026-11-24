// local_stream_cap_test — #108: a local model server can stream one reply for
// longer than the hosted 30-minute cap. Drives the real HTTP client against a
// loopback server that trickles SSE, with the clock shrunk to seconds: the
// hosted ladder shape (a total cap) cuts it, the local ladder (no total cap)
// lets it finish.

#include <maya/core/target.hpp>
#include "agtest.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "agentty/io/http.hpp"
#include "agentty/provider/stream_scaffold.hpp"
#include "agentty/util/io.hpp"

#if MAYA_OS_WINDOWS
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace agentty;
using namespace std::chrono_literals;

namespace {

#if MAYA_OS_WINDOWS
using socket_t = SOCKET;
constexpr socket_t kInvalid = INVALID_SOCKET;
void close_socket(socket_t s) { if (s != kInvalid) ::closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kInvalid = -1;
void close_socket(socket_t s) { if (s != kInvalid) ::close(s); }
#endif

// One connection: answers 200 and sends a token every `gap`, `n` times.
class TrickleServer {
public:
    TrickleServer(int n, std::chrono::milliseconds gap) : n_(n), gap_(gap) {
#if MAYA_OS_WINDOWS
        WSADATA d{};
        (void)::WSAStartup(MAKEWORD(2, 2), &d);
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd_ == kInvalid
            || ::bind(fd_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0
            || ::listen(fd_, 4) != 0) {
            close_socket(fd_);
            fd_ = kInvalid;
            return;
        }
#if MAYA_OS_WINDOWS
        int len = sizeof(a);
#else
        socklen_t len = sizeof(a);
#endif
        (void)::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        worker_ = std::thread([this] { serve(); });
    }
    ~TrickleServer() {
        stop_.store(true);
        close_socket(fd_);   // unblocks accept() if the client never came
        if (worker_.joinable()) worker_.join();
#if MAYA_OS_WINDOWS
        ::WSACleanup();
#endif
    }
    [[nodiscard]] bool valid() const { return fd_ != kInvalid; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

private:
    void send_all(socket_t c, const std::string& s) {
        std::size_t off = 0;
        while (off < s.size()) {
#if MAYA_OS_WINDOWS
            const auto r = ::send(c, s.data() + off, static_cast<int>(s.size() - off), 0);
#else
            // The capped client hangs up mid-stream; that must not SIGPIPE
            // the whole test binary.
            const auto r = ::send(c, s.data() + off, s.size() - off, MSG_NOSIGNAL);
#endif
            if (r <= 0) return;
            off += static_cast<std::size_t>(r);
        }
    }
    void serve() {
        const socket_t c = ::accept(fd_, nullptr, nullptr);
        if (c == kInvalid) return;
        char buf[8192];
        (void)::recv(c, buf, sizeof(buf), 0);   // the request; content is irrelevant
        send_all(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                    "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
        auto chunk = [](const std::string& s) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "%zx", s.size());
            return std::string{hex} + "\r\n" + s + "\r\n";
        };
        for (int i = 0; i < n_; ++i) {
            // Sleep in steps so a test whose client already gave up doesn't
            // hold the destructor's join for the whole gap.
            const auto until = std::chrono::steady_clock::now() + gap_;
            while (!stop_.load() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(20ms);
            if (stop_.load()) break;
            send_all(c, chunk("data: tok\n\n"));
        }
        if (!stop_.load()) send_all(c, chunk("data: [DONE]\n\n") + "0\r\n\r\n");
        close_socket(c);
    }

    int n_;
    std::chrono::milliseconds gap_;
    socket_t fd_ = kInvalid;
    std::uint16_t port_ = 0;
    std::thread worker_;
    std::atomic<bool> stop_{false};
};

struct Outcome { bool ok; int tokens; std::string err; };

Outcome run(std::uint16_t port, http::Timeouts tos) {
    http::Request req;
    req.method    = http::HttpMethod::Post;
    req.host      = "127.0.0.1";
    req.port      = port;
    req.path      = "/v1/chat/completions";
    req.plaintext = true;
    req.body      = "{}";
    int tokens = 0;
    http::StreamHandler h;
    h.on_headers = [](int, const http::Headers&) {};
    h.on_chunk   = [&](std::string_view s) {
        for (auto p = s.find("tok"); p != std::string_view::npos; p = s.find("tok", p + 1))
            ++tokens;
        return true;
    };
    auto r = http::default_client(IoAccess::grant()).stream(req, std::move(h), tos, {});
    return {bool(r), tokens, r ? std::string{} : r.error().render()};
}

// The real ladders, with every duration scaled from minutes to tenths of a
// second so the test runs in about a second. idle stays well above the gap.
http::Timeouts shrink(http::Timeouts t) {
    if (t.total.count() > 0) t.total = 600ms;   // "30 min" -> 0.6 s
    t.idle = 2s;
    t.ping = 0ms;
    return t;
}

} // namespace

TEST_CASE("local stream: no total cap, so a long reply finishes (#108)") {
    // 10 tokens, 0.1 s apart: ~1 s of streaming, past the shrunk 0.6 s cap.
    TrickleServer s(10, 100ms);
    REQUIRE(s.valid(), "loopback server starts");
    const auto out = run(s.port(), shrink(provider::local_stream_timeouts()));
    CHECK(out.ok);
    CHECK(out.tokens == 10);
}

TEST_CASE("local stream: the hosted ladder's cap would have cut it") {
    // The control: same server, hosted ladder shape. This is what #108 hit.
    TrickleServer s(10, 100ms);
    REQUIRE(s.valid(), "loopback server starts");
    const auto out = run(s.port(), shrink(provider::stream_timeouts()));
    CHECK(!out.ok);
    CHECK(out.err.find("deadline exceeded") != std::string::npos);
    CHECK(out.tokens < 10);
}

TEST_CASE("local stream: a silent server is still caught by idle") {
    // No total cap must not mean hanging forever. The server goes quiet for
    // 5 s; the 2 s idle guard has to end it, well before the server would.
    TrickleServer s(1, 5000ms);
    REQUIRE(s.valid(), "loopback server starts");
    const auto t0 = std::chrono::steady_clock::now();
    const auto out = run(s.port(), shrink(provider::local_stream_timeouts()));
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(!out.ok);
    CHECK(out.err.find("idle") != std::string::npos);
    CHECK(took < 3500ms);
}
