// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/grpc.h>

#include <nghttp2/nghttp2.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static constexpr sock_t BAD_SOCK = INVALID_SOCKET;
static void sock_close(sock_t s) { closesocket(s); }
#define POLLFD WSAPOLLFD
#define poll_impl WSAPoll
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static constexpr sock_t BAD_SOCK = -1;
static void sock_close(sock_t s) { ::close(s); }
#define POLLFD pollfd
#define poll_impl ::poll
#endif

namespace bt::grpc {

std::optional<Url> Url::parse(const std::string& s)
{
    Url u;
    std::string rest;
    if (s.rfind("https://", 0) == 0) {
        u.tls = true;
        rest = s.substr(8);
    } else if (s.rfind("http://", 0) == 0) {
        rest = s.substr(7);
    } else {
        return std::nullopt;
    }
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    if (slash != std::string::npos) {
        u.path_prefix = rest.substr(slash);
        while (!u.path_prefix.empty() && u.path_prefix.back() == '/') u.path_prefix.pop_back();
    }
    if (!hostport.empty() && hostport[0] == '[') {
        auto e = hostport.find(']');
        if (e == std::string::npos) return std::nullopt;
        u.host = hostport.substr(1, e - 1);
        if (e + 1 < hostport.size() && hostport[e + 1] == ':') u.port = uint16_t(std::stoi(hostport.substr(e + 2)));
    } else {
        auto c = hostport.rfind(':');
        u.host = hostport.substr(0, c);
        if (c != std::string::npos) u.port = uint16_t(std::stoi(hostport.substr(c + 1)));
    }
    if (!u.port) u.port = u.tls ? 443 : 80;
    if (u.host.empty()) return std::nullopt;
    return u;
}

struct StreamState {
    Bytes body;          // request body (framed)
    size_t sent = 0;
    Bytes rx;            // response data accumulator
    bool closed = false;
    int status = -1;
    std::string message;
    int http_status = 0;
    const std::function<bool(const Bytes&)>* on_message = nullptr;
    bool cancelled = false;
    std::string transport_error;
};

struct Channel::Conn {
    Url url;
    sock_t fd = BAD_SOCK;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    nghttp2_session* sess = nullptr;
    StreamState* cur = nullptr;
    bool broken = false;
    ~Conn()
    {
        if (sess) nghttp2_session_del(sess);
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        if (fd != BAD_SOCK) sock_close(fd);
    }
    bool io_write(const uint8_t* d, size_t n)
    {
        while (n) {
            int w;
            if (ssl) {
                w = SSL_write(ssl, d, int(n));
                if (w <= 0) return false;
            } else {
                w = int(::send(fd, (const char*)d, n, 0));
                if (w <= 0) return false;
            }
            d += w;
            n -= size_t(w);
        }
        return true;
    }
    long io_read(uint8_t* d, size_t n)
    {
        if (ssl) {
            int r = SSL_read(ssl, d, int(n));
            if (r <= 0) {
                int e = SSL_get_error(ssl, r);
                if (e == SSL_ERROR_WANT_READ) return 0;
                return -1;
            }
            return r;
        }
        auto r = ::recv(fd, (char*)d, n, 0);
        return r <= 0 ? -1 : long(r);
    }
    bool flush()
    {
        for (;;) {
            const uint8_t* data;
            ssize_t n = nghttp2_session_mem_send(sess, &data);
            if (n < 0) return false;
            if (n == 0) return true;
            if (!io_write(data, size_t(n))) return false;
        }
    }
};

static ssize_t read_body_cb(nghttp2_session*, int32_t, uint8_t* buf, size_t len, uint32_t* flags, nghttp2_data_source* src, void*)
{
    auto* st = (StreamState*)src->ptr;
    size_t n = std::min(len, st->body.size() - st->sent);
    std::memcpy(buf, st->body.data() + st->sent, n);
    st->sent += n;
    if (st->sent == st->body.size()) *flags |= NGHTTP2_DATA_FLAG_EOF;
    return ssize_t(n);
}

static int on_header_cb(nghttp2_session*, const nghttp2_frame*, const uint8_t* name, size_t nl, const uint8_t* value, size_t vl, uint8_t, void* ud)
{
    auto* c = (Channel::Conn*)ud;
    if (!c->cur) return 0;
    std::string n((const char*)name, nl), v((const char*)value, vl);
    if (n == ":status") c->cur->http_status = std::atoi(v.c_str());
    else if (n == "grpc-status") c->cur->status = std::atoi(v.c_str());
    else if (n == "grpc-message") c->cur->message = v;
    return 0;
}

static void deliver_frames(StreamState* st)
{
    // gRPC length-prefixed messages: flag(1) || len(4, BE) || message
    while (st->rx.size() >= 5) {
        uint32_t len = uint32_t(st->rx[1]) << 24 | uint32_t(st->rx[2]) << 16 | uint32_t(st->rx[3]) << 8 | st->rx[4];
        if (st->rx.size() < 5 + size_t(len)) return;
        if (st->rx[0] != 0) { // compressed: we never request compression
            st->transport_error = "compressed gRPC message not supported";
            st->cancelled = true;
            return;
        }
        Bytes m(st->rx.begin() + 5, st->rx.begin() + 5 + len);
        st->rx.erase(st->rx.begin(), st->rx.begin() + 5 + len);
        if (st->on_message && !(*st->on_message)(m)) {
            st->cancelled = true;
            return;
        }
    }
}

static int on_data_cb(nghttp2_session* s, uint8_t, int32_t sid, const uint8_t* data, size_t len, void* ud)
{
    auto* c = (Channel::Conn*)ud;
    if (!c->cur) return 0;
    c->cur->rx.insert(c->cur->rx.end(), data, data + len);
    if (c->cur->rx.size() > (1ull << 30)) return NGHTTP2_ERR_CALLBACK_FAILURE; // 1 GiB, like their max decode size
    deliver_frames(c->cur);
    if (c->cur->cancelled) nghttp2_submit_rst_stream(s, NGHTTP2_FLAG_NONE, sid, NGHTTP2_CANCEL);
    return 0;
}

static int on_close_cb(nghttp2_session*, int32_t, uint32_t err, void* ud)
{
    auto* c = (Channel::Conn*)ud;
    if (c->cur) {
        c->cur->closed = true;
        if (err && c->cur->status < 0 && !c->cur->cancelled) c->cur->transport_error = "stream reset (" + std::to_string(err) + ")";
    }
    return 0;
}

Channel::Channel(const std::string& url, std::string ca_file) : url_(url), ca_(std::move(ca_file)) {}
Channel::~Channel() = default;

static std::unique_ptr<Channel::Conn> open_conn(const Url& u, const std::string& ca, std::string& err)
{
    auto c = std::make_unique<Channel::Conn>();
    c->url = u;
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0) {
        err = "cannot resolve " + u.host;
        return nullptr;
    }
    for (auto* p = res; p; p = p->ai_next) {
        sock_t fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == BAD_SOCK) continue;
        if (::connect(fd, p->ai_addr, (int)p->ai_addrlen) == 0) {
            c->fd = fd;
            break;
        }
        sock_close(fd);
    }
    freeaddrinfo(res);
    if (c->fd == BAD_SOCK) {
        err = "cannot connect to " + u.host + ":" + std::to_string(u.port);
        return nullptr;
    }
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    if (u.tls) {
        c->ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
        bool loaded = false;
        if (!ca.empty()) loaded = SSL_CTX_load_verify_locations(c->ctx, ca.c_str(), nullptr) == 1;
        if (!loaded) {
            SSL_CTX_set_default_verify_paths(c->ctx);
            for (const char* f : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"})
                if (SSL_CTX_load_verify_locations(c->ctx, f, nullptr) == 1) break;
        }
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, nullptr);
        static const unsigned char alpn[] = {2, 'h', '2'};
        SSL_CTX_set_alpn_protos(c->ctx, alpn, sizeof(alpn));
        c->ssl = SSL_new(c->ctx);
        SSL_set_tlsext_host_name(c->ssl, u.host.c_str());
        SSL_set1_host(c->ssl, u.host.c_str());
        SSL_set_fd(c->ssl, int(c->fd));
        if (SSL_connect(c->ssl) != 1) {
            char buf[256];
            ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
            err = std::string("TLS handshake failed: ") + buf;
            return nullptr;
        }
        const unsigned char* sel = nullptr;
        unsigned sl = 0;
        SSL_get0_alpn_selected(c->ssl, &sel, &sl);
        if (sl != 2 || std::memcmp(sel, "h2", 2) != 0) {
            err = "server did not negotiate HTTP/2";
            return nullptr;
        }
    }
    nghttp2_session_callbacks* cbs;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_close_cb);
    nghttp2_session_client_new(&c->sess, cbs, c.get());
    nghttp2_session_callbacks_del(cbs);
    nghttp2_settings_entry iv[2] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}, {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1 << 24}};
    nghttp2_submit_settings(c->sess, NGHTTP2_FLAG_NONE, iv, 2);
    nghttp2_session_set_local_window_size(c->sess, NGHTTP2_FLAG_NONE, 0, 1 << 26);
    if (!c->flush()) {
        err = "HTTP/2 preface failed";
        return nullptr;
    }
    return c;
}

Status Channel::run(const std::string& method, const Bytes& request, const std::function<bool(const Bytes&)>& on_message, int timeout_ms,
                    const std::atomic<bool>* cancel)
{
    auto u = Url::parse(url_);
    if (!u) return {-1, "bad URL " + url_};
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!conn_ || conn_->broken) {
            std::string err;
            conn_ = open_conn(*u, ca_, err);
            if (!conn_) return {-1, err};
        }
        Conn& c = *conn_;
        StreamState st;
        st.on_message = &on_message;
        st.body.reserve(5 + request.size());
        st.body.push_back(0);
        for (int i = 3; i >= 0; i--) st.body.push_back(uint8_t(request.size() >> (8 * i)));
        st.body.insert(st.body.end(), request.begin(), request.end());
        std::string path = u->path_prefix + method;
        std::string authority = u->host + ":" + std::to_string(u->port);
        std::string scheme = u->tls ? "https" : "http";
        auto nv = [](const char* n, const std::string& v) {
            return nghttp2_nv{(uint8_t*)n, (uint8_t*)v.data(), strlen(n), v.size(), NGHTTP2_NV_FLAG_NONE};
        };
        std::string post = "POST", ct = "application/grpc", te = "trailers", ua = "backbone-thunder/0.1 grpc-c++-mini";
        nghttp2_nv hdrs[] = {nv(":method", post), nv(":scheme", scheme), nv(":authority", authority), nv(":path", path),
                             nv("content-type", ct), nv("te", te), nv("user-agent", ua)};
        nghttp2_data_provider prd;
        prd.source.ptr = &st;
        prd.read_callback = read_body_cb;
        c.cur = &st;
        int32_t sid = nghttp2_submit_request(c.sess, nullptr, hdrs, sizeof(hdrs) / sizeof(hdrs[0]), &prd, nullptr);
        if (sid < 0 || !c.flush()) {
            c.cur = nullptr;
            c.broken = true;
            continue; // reconnect once
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        uint8_t buf[65536];
        while (!st.closed) {
            if (cancel && cancel->load()) {
                nghttp2_submit_rst_stream(c.sess, NGHTTP2_FLAG_NONE, sid, NGHTTP2_CANCEL);
                c.flush();
                st.cancelled = true;
            }
            if (timeout_ms > 0 && std::chrono::steady_clock::now() > deadline) {
                nghttp2_submit_rst_stream(c.sess, NGHTTP2_FLAG_NONE, sid, NGHTTP2_CANCEL);
                c.flush();
                c.cur = nullptr;
                return {-1, "deadline exceeded"};
            }
            bool pending_tls = c.ssl && SSL_pending(c.ssl) > 0;
            if (!pending_tls) {
                POLLFD p{};
                p.fd = c.fd;
                p.events = POLLIN;
                int r = poll_impl(&p, 1, 500);
                if (r <= 0) continue;
            }
            long n = c.io_read(buf, sizeof(buf));
            if (n == 0) continue;
            if (n < 0) {
                c.broken = true;
                break;
            }
            if (nghttp2_session_mem_recv(c.sess, buf, size_t(n)) < 0) {
                c.broken = true;
                break;
            }
            if (!c.flush()) {
                c.broken = true;
                break;
            }
            if (!nghttp2_session_want_read(c.sess) && !nghttp2_session_want_write(c.sess)) c.broken = true;
        }
        c.cur = nullptr;
        if (!st.closed) {
            if (attempt == 0 && st.rx.empty() && st.status < 0) continue; // stale connection: retry once
            return {-1, "connection lost"};
        }
        if (!st.transport_error.empty() && st.status < 0) return {-1, st.transport_error};
        if (st.cancelled && st.status < 0) return {1, "cancelled"};
        if (st.status < 0) return {-1, "no grpc-status (HTTP " + std::to_string(st.http_status) + ")"};
        return {st.status, st.message};
    }
    return {-1, "connection failed"};
}

Status Channel::call(const std::string& method, const Bytes& request, Bytes& response, int timeout_ms)
{
    std::lock_guard<std::mutex> l(mu_);
    bool got = false;
    std::function<bool(const Bytes&)> f = [&](const Bytes& m) {
        response = m;
        got = true;
        return true;
    };
    Status s = run(method, request, f, timeout_ms, nullptr);
    if (s.ok() && !got) return {-1, "empty unary response"};
    return s;
}

Status Channel::stream(const std::string& method, const Bytes& request, const std::function<bool(const Bytes&)>& on_message,
                       const std::atomic<bool>* cancel)
{
    std::lock_guard<std::mutex> l(mu_);
    return run(method, request, on_message, 0, cancel);
}

} // namespace bt::grpc
