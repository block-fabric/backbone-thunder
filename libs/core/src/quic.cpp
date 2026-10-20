// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/quic.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
using sock_t = SOCKET;
static constexpr sock_t BAD_SOCK = INVALID_SOCKET;
static int sock_close(sock_t s) { return closesocket(s); }
static int poll_fds(WSAPOLLFD* f, unsigned n, int ms) { return WSAPoll(f, n, ms); }
using pollfd_t = WSAPOLLFD;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static constexpr sock_t BAD_SOCK = -1;
static int sock_close(sock_t s) { return ::close(s); }
static int poll_fds(pollfd* f, unsigned n, int ms) { return ::poll(f, n, ms); }
using pollfd_t = pollfd;
#endif

namespace bt::quic {

static constexpr size_t SCID_LEN = 16;

std::string SockAddr::to_string() const
{
    if (ip.find(':') != std::string::npos) return "[" + ip + "]:" + std::to_string(port);
    return ip + ":" + std::to_string(port);
}

static bool to_sockaddr(const SockAddr& a, sockaddr_storage& ss, socklen_t& len)
{
    std::memset(&ss, 0, sizeof(ss));
    auto* v4 = (sockaddr_in*)&ss;
    if (inet_pton(AF_INET, a.ip.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(a.port);
        len = sizeof(sockaddr_in);
        return true;
    }
    auto* v6 = (sockaddr_in6*)&ss;
    if (inet_pton(AF_INET6, a.ip.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(a.port);
        len = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

static SockAddr from_sockaddr(const sockaddr* sa)
{
    char buf[INET6_ADDRSTRLEN] = {0};
    SockAddr r;
    if (sa->sa_family == AF_INET) {
        auto* v4 = (const sockaddr_in*)sa;
        inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof(buf));
        r.port = ntohs(v4->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        auto* v6 = (const sockaddr_in6*)sa;
        inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof(buf));
        r.port = ntohs(v6->sin6_port);
    }
    r.ip = buf;
    return r;
}

std::vector<SockAddr> resolve(const std::string& host, uint16_t port)
{
    std::vector<SockAddr> v4, v6;
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return {};
    for (auto* p = res; p; p = p->ai_next) {
        SockAddr a = from_sockaddr(p->ai_addr);
        a.port = port;
        if (a.ip.empty()) continue;
        auto& dst = p->ai_family == AF_INET ? v4 : v6;
        if (std::find(dst.begin(), dst.end(), a) == dst.end()) dst.push_back(a);
    }
    freeaddrinfo(res);
    v4.insert(v4.end(), v6.begin(), v6.end());
    return v4;
}

static uint64_t now_ns()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Stream {
    int64_t id = -1;
    bool local = false;
    Bytes send_buf;
    size_t sent = 0;
    bool fin_pending = false, fin_sent = false;
    Bytes recv_buf;
    size_t recv_limit = 0;
    bool overflow = false, delivered = false;
    ResponseFn on_response;
    uint64_t deadline = 0;
};

struct PendingSend {
    Bytes payload;
    size_t limit;
    std::chrono::milliseconds timeout;
    ResponseFn fn;
};

struct Conn;

struct Endpoint::Impl {
    Callbacks cb;
    Options opt;
    sock_t sock = BAD_SOCK, wake_rx = BAD_SOCK;
    sockaddr_storage local_ss{};
    socklen_t local_len = 0;
    SockAddr local;
    std::thread thr;
    std::atomic<bool> running{false};
    SSL_CTX* client_ctx = nullptr;
    SSL_CTX* server_ctx = nullptr;
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    std::mutex mu;
    std::deque<std::function<void()>> cmds;
    sockaddr_storage wake_ss{};
    socklen_t wake_len = 0;
    std::map<ConnId, std::unique_ptr<Conn>> conns;
    std::unordered_map<std::string, ConnId> cid_map;
    std::atomic<ConnId> next_id{1};

    void post(std::function<void()> f)
    {
        {
            std::lock_guard<std::mutex> l(mu);
            cmds.push_back(std::move(f));
        }
        if (wake_rx != BAD_SOCK) {
            char b = 1;
            sendto(wake_rx, &b, 1, 0, (sockaddr*)&wake_ss, wake_len);
        }
    }
    void loop();
    void on_packet(const uint8_t* data, size_t n, const sockaddr* from, socklen_t fromlen);
    Conn* new_conn(bool inbound, const sockaddr* remote, socklen_t rlen, const ngtcp2_pkt_hd* hd, const std::string& sni, ConnId id);
    void write_conn(Conn& c);
    void drop(ConnId id, const std::string& reason, bool send_close);
    bool make_cert();
};

struct Conn {
    Endpoint::Impl* ep = nullptr;
    ConnId id = 0;
    ngtcp2_conn* conn = nullptr;
    ngtcp2_crypto_ossl_ctx* ossl = nullptr;
    SSL* ssl = nullptr;
    ngtcp2_crypto_conn_ref ref{};
    sockaddr_storage remote{};
    socklen_t remote_len = 0;
    SockAddr remote_s;
    bool inbound = false, handshake_done = false, dead = false;
    std::map<int64_t, Stream> streams;
    std::deque<PendingSend> pending;
    std::vector<std::string> cids;
    ngtcp2_ccerr ccerr{};
    ~Conn()
    {
        if (conn) ngtcp2_conn_del(conn);
        if (ossl) ngtcp2_crypto_ossl_ctx_del(ossl);
        if (ssl) SSL_free(ssl);
    }
    ngtcp2_path path() const
    {
        ngtcp2_path p;
        p.local.addr = (ngtcp2_sockaddr*)&ep->local_ss;
        p.local.addrlen = ep->local_len;
        p.remote.addr = (ngtcp2_sockaddr*)&remote;
        p.remote.addrlen = remote_len;
        p.user_data = nullptr;
        return p;
    }
    void open_pending()
    {
        while (!pending.empty()) {
            int64_t sid;
            if (ngtcp2_conn_open_bidi_stream(conn, &sid, nullptr) != 0) return; // wait for credit
            PendingSend ps = std::move(pending.front());
            pending.pop_front();
            Stream s;
            s.id = sid;
            s.local = true;
            s.send_buf = std::move(ps.payload);
            s.fin_pending = true;
            s.recv_limit = ps.limit;
            s.on_response = std::move(ps.fn);
            s.deadline = ps.limit ? now_ns() + uint64_t(ps.timeout.count()) * 1000000ull : 0;
            if (!ps.limit) s.delivered = true; // no response expected
            streams.emplace(sid, std::move(s));
        }
    }
};

static std::string cid_key(const uint8_t* d, size_t n) { return std::string((const char*)d, n); }

// ---------------------------------------------------------------- ngtcp2 callbacks
static ngtcp2_conn* get_conn_cb(ngtcp2_crypto_conn_ref* ref) { return ((Conn*)ref->user_data)->conn; }

static void rand_cb(uint8_t* dest, size_t len, const ngtcp2_rand_ctx*) { RAND_bytes(dest, int(len)); }

static int new_cid_cb(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, size_t cidlen, void* ud)
{
    auto* c = (Conn*)ud;
    if (RAND_bytes(cid->data, int(cidlen)) != 1) return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = cidlen;
    if (RAND_bytes(token->data, sizeof(token->data)) != 1) return NGTCP2_ERR_CALLBACK_FAILURE;
    auto k = cid_key(cid->data, cidlen);
    c->ep->cid_map[k] = c->id;
    c->cids.push_back(k);
    return 0;
}

static int remove_cid_cb(ngtcp2_conn*, const ngtcp2_cid* cid, void* ud)
{
    auto* c = (Conn*)ud;
    c->ep->cid_map.erase(cid_key(cid->data, cid->datalen));
    return 0;
}

static int handshake_completed_cb(ngtcp2_conn*, void* ud)
{
    auto* c = (Conn*)ud;
    c->handshake_done = true;
    if (c->ep->cb.on_connected) c->ep->cb.on_connected(c->id, c->remote_s, c->inbound);
    c->open_pending();
    return 0;
}

static int stream_open_cb(ngtcp2_conn*, int64_t sid, void* ud)
{
    auto* c = (Conn*)ud;
    Stream s;
    s.id = sid;
    s.local = false;
    s.recv_limit = c->ep->opt.max_request_bytes;
    c->streams.emplace(sid, std::move(s));
    return 0;
}

static int recv_stream_data_cb(ngtcp2_conn* conn, uint32_t flags, int64_t sid, uint64_t, const uint8_t* data, size_t len, void* ud, void*)
{
    auto* c = (Conn*)ud;
    ngtcp2_conn_extend_max_stream_offset(conn, sid, len);
    ngtcp2_conn_extend_max_offset(conn, len);
    auto it = c->streams.find(sid);
    if (it == c->streams.end()) return 0;
    Stream& s = it->second;
    if (s.overflow || s.delivered) return 0;
    if (s.recv_buf.size() + len > s.recv_limit) {
        s.overflow = true;
        ngtcp2_conn_shutdown_stream(conn, 0, sid, 1);
        if (s.local && s.on_response) {
            s.delivered = true;
            s.on_response(std::nullopt);
        }
        return 0;
    }
    s.recv_buf.insert(s.recv_buf.end(), data, data + len);
    if (flags & NGTCP2_STREAM_DATA_FLAG_FIN) {
        s.delivered = true;
        if (s.local) {
            if (s.on_response) s.on_response(std::move(s.recv_buf));
        } else if (c->ep->cb.on_request) {
            c->ep->cb.on_request(c->id, sid, std::move(s.recv_buf));
        }
        s.recv_buf.clear();
    }
    return 0;
}

static int stream_close_cb(ngtcp2_conn* conn, uint32_t, int64_t sid, uint64_t, void* ud, void*)
{
    auto* c = (Conn*)ud;
    auto it = c->streams.find(sid);
    if (it != c->streams.end()) {
        Stream& s = it->second;
        if (s.local && !s.delivered && s.on_response) s.on_response(std::nullopt);
        bool remote = !s.local;
        c->streams.erase(it);
        if (remote) ngtcp2_conn_extend_max_streams_bidi(conn, 1);
    }
    return 0;
}

static int stream_reset_cb(ngtcp2_conn*, int64_t sid, uint64_t, uint64_t, void* ud, void*)
{
    auto* c = (Conn*)ud;
    auto it = c->streams.find(sid);
    if (it != c->streams.end() && it->second.local && !it->second.delivered) {
        it->second.delivered = true;
        if (it->second.on_response) it->second.on_response(std::nullopt);
    }
    return 0;
}

static int extend_bidi_cb(ngtcp2_conn*, uint64_t, void* ud)
{
    ((Conn*)ud)->open_pending();
    return 0;
}

static ngtcp2_callbacks make_callbacks(bool server)
{
    ngtcp2_callbacks cb{};
    if (server) cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    else {
        cb.client_initial = ngtcp2_crypto_client_initial_cb;
        cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
    }
    cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cb.encrypt = ngtcp2_crypto_encrypt_cb;
    cb.decrypt = ngtcp2_crypto_decrypt_cb;
    cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cb.update_key = ngtcp2_crypto_update_key_cb;
    cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    cb.rand = rand_cb;
    cb.get_new_connection_id2 = new_cid_cb;
    cb.remove_connection_id = remove_cid_cb;
    cb.handshake_completed = handshake_completed_cb;
    cb.stream_open = stream_open_cb;
    cb.recv_stream_data = recv_stream_data_cb;
    cb.stream_close = stream_close_cb;
    cb.stream_reset = stream_reset_cb;
    cb.extend_max_local_streams_bidi = extend_bidi_cb;
    return cb;
}

// ---------------------------------------------------------------- Endpoint
Endpoint::Endpoint(Callbacks cb, Options opt) : m(new Impl)
{
    m->cb = std::move(cb);
    m->opt = std::move(opt);
}

Endpoint::~Endpoint() { stop(); }

bool Endpoint::Impl::make_cert()
{
    key = EVP_EC_gen("P-256");
    if (!key) return false;
    cert = X509_new();
    X509_set_version(cert, 2);
    uint8_t serial[16];
    RAND_bytes(serial, sizeof(serial));
    serial[0] &= 0x7f;
    BIGNUM* bn = BN_bin2bn(serial, sizeof(serial), nullptr);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert));
    BN_free(bn);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 365 * 4000 / 1000); // ~4 years
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"rcgen self signed cert", -1, -1, 0);
    X509_set_issuer_name(cert, name);
    std::string san = "DNS:localhost";
    for (auto& n : opt.server_names)
        if (n != "localhost") san += ",DNS:" + n;
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, san.c_str());
    if (ext) {
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }
    return X509_sign(cert, key, EVP_sha256()) > 0;
}

bool Endpoint::start(const SockAddr& bind_addr, std::string& err)
{
#ifdef _WIN32
    static std::once_flag wsa;
    std::call_once(wsa, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
#endif
    if (ngtcp2_crypto_ossl_init() != 0) {
        err = "ngtcp2_crypto_ossl_init failed";
        return false;
    }
    if (!m->make_cert()) {
        err = "certificate generation failed";
        return false;
    }
    m->client_ctx = SSL_CTX_new(TLS_client_method());
    m->server_ctx = SSL_CTX_new(TLS_server_method());
    if (!m->client_ctx || !m->server_ctx) {
        err = "SSL_CTX_new failed";
        return false;
    }
    for (auto* ctx : {m->client_ctx, m->server_ctx}) {
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    }
    SSL_CTX_set_verify(m->client_ctx, SSL_VERIFY_NONE, nullptr); // their SkipServerVerification
    if (SSL_CTX_use_certificate(m->server_ctx, m->cert) != 1 || SSL_CTX_use_PrivateKey(m->server_ctx, m->key) != 1) {
        err = "server certificate setup failed";
        return false;
    }

    if (!to_sockaddr(bind_addr, m->local_ss, m->local_len)) {
        err = "bad bind address " + bind_addr.ip;
        return false;
    }
    m->sock = socket(m->local_ss.ss_family, SOCK_DGRAM, 0);
    if (m->sock == BAD_SOCK) {
        err = "socket() failed";
        return false;
    }
    if (::bind(m->sock, (sockaddr*)&m->local_ss, m->local_len) != 0) {
        err = "bind " + bind_addr.to_string() + " failed";
        sock_close(m->sock);
        m->sock = BAD_SOCK;
        return false;
    }
    socklen_t l = sizeof(m->local_ss);
    getsockname(m->sock, (sockaddr*)&m->local_ss, &l);
    m->local_len = l;
    m->local = from_sockaddr((sockaddr*)&m->local_ss);
    // loopback wake socket
    m->wake_rx = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in w{};
    w.sin_family = AF_INET;
    w.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(m->wake_rx, (sockaddr*)&w, sizeof(w));
    m->wake_len = sizeof(m->wake_ss);
    getsockname(m->wake_rx, (sockaddr*)&m->wake_ss, &m->wake_len);
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(m->sock, FIONBIO, &nb);
    ioctlsocket(m->wake_rx, FIONBIO, &nb);
#else
    fcntl(m->sock, F_SETFL, fcntl(m->sock, F_GETFL) | O_NONBLOCK);
    fcntl(m->wake_rx, F_SETFL, fcntl(m->wake_rx, F_GETFL) | O_NONBLOCK);
#endif
    m->running = true;
    m->thr = std::thread([this] { m->loop(); });
    return true;
}

void Endpoint::stop()
{
    if (!m) return;
    if (m->running.exchange(false)) {
        m->post([] {});
        if (m->thr.joinable()) m->thr.join();
    }
    m->conns.clear();
    if (m->sock != BAD_SOCK) sock_close(m->sock);
    if (m->wake_rx != BAD_SOCK) sock_close(m->wake_rx);
    m->sock = m->wake_rx = BAD_SOCK;
    if (m->client_ctx) SSL_CTX_free(m->client_ctx);
    if (m->server_ctx) SSL_CTX_free(m->server_ctx);
    if (m->cert) X509_free(m->cert);
    if (m->key) EVP_PKEY_free(m->key);
    m->client_ctx = m->server_ctx = nullptr;
    m->cert = nullptr;
    m->key = nullptr;
}

SockAddr Endpoint::local_address() const { return m->local; }

Bytes Endpoint::certificate_der() const
{
    unsigned char* p = nullptr;
    int n = i2d_X509(m->cert, &p);
    Bytes r(p, p + (n > 0 ? n : 0));
    OPENSSL_free(p);
    return r;
}

Conn* Endpoint::Impl::new_conn(bool inbound, const sockaddr* remote, socklen_t rlen, const ngtcp2_pkt_hd* hd, const std::string& sni, ConnId id)
{
    auto c = std::make_unique<Conn>();
    c->ep = this;
    c->id = id;
    c->inbound = inbound;
    std::memcpy(&c->remote, remote, size_t(rlen));
    c->remote_len = rlen;
    c->remote_s = from_sockaddr(remote);
    c->ref.get_conn = get_conn_cb;
    c->ref.user_data = c.get();

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now_ns();
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi = 100; // quinn default
    params.initial_max_streams_uni = 1;    // their max_concurrent_uni_streams(1)
    params.initial_max_stream_data_bidi_local = 16 * 1024 * 1024;
    params.initial_max_stream_data_bidi_remote = 1024 * 1024;
    params.initial_max_stream_data_uni = 64 * 1024;
    params.initial_max_data = 32 * 1024 * 1024;
    params.max_idle_timeout = uint64_t(opt.idle_timeout.count()) * NGTCP2_MILLISECONDS;

    ngtcp2_cid scid;
    scid.datalen = SCID_LEN;
    RAND_bytes(scid.data, SCID_LEN);
    ngtcp2_path path = c->path();
    ngtcp2_callbacks cbs = make_callbacks(inbound);
    int rv;
    if (inbound) {
        params.original_dcid = hd->dcid;
        params.original_dcid_present = 1;
        rv = ngtcp2_conn_server_new(&c->conn, &hd->scid, &scid, &path, hd->version, &cbs, &settings, &params, nullptr, c.get());
    } else {
        ngtcp2_cid dcid;
        dcid.datalen = 18;
        RAND_bytes(dcid.data, int(dcid.datalen));
        rv = ngtcp2_conn_client_new(&c->conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &cbs, &settings, &params, nullptr, c.get());
    }
    if (rv != 0) return nullptr;
    c->ssl = SSL_new(inbound ? server_ctx : client_ctx);
    if (!c->ssl || ngtcp2_crypto_ossl_ctx_new(&c->ossl, c->ssl) != 0) return nullptr;
    if (inbound) {
        if (ngtcp2_crypto_ossl_configure_server_session(c->ssl) != 0) return nullptr;
        SSL_set_app_data(c->ssl, &c->ref);
        SSL_set_accept_state(c->ssl);
    } else {
        if (ngtcp2_crypto_ossl_configure_client_session(c->ssl) != 0) return nullptr;
        SSL_set_app_data(c->ssl, &c->ref);
        SSL_set_connect_state(c->ssl);
        SSL_set_tlsext_host_name(c->ssl, sni.c_str()); // no ALPN: as quinn/rustls with empty alpn_protocols
    }
    ngtcp2_conn_set_tls_native_handle(c->conn, c->ossl);
    auto k = cid_key(scid.data, scid.datalen);
    cid_map[k] = id;
    c->cids.push_back(k);
    if (inbound) {
        auto k2 = cid_key(hd->dcid.data, hd->dcid.datalen);
        cid_map[k2] = id;
        c->cids.push_back(k2);
    }
    Conn* raw = c.get();
    conns[id] = std::move(c);
    return raw;
}

void Endpoint::Impl::drop(ConnId id, const std::string& reason, bool send_close)
{
    auto it = conns.find(id);
    if (it == conns.end()) return;
    Conn& c = *it->second;
    if (send_close && c.conn && !ngtcp2_conn_in_closing_period(c.conn) && !ngtcp2_conn_in_draining_period(c.conn)) {
        uint8_t buf[1500];
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi;
        if (c.ccerr.type == 0 && c.ccerr.error_code == 0) ngtcp2_ccerr_default(&c.ccerr);
        auto n = ngtcp2_conn_write_connection_close(c.conn, &ps.path, &pi, buf, sizeof(buf), &c.ccerr, now_ns());
        if (n > 0) sendto(sock, (const char*)buf, size_t(n), 0, (sockaddr*)&c.remote, c.remote_len);
    }
    for (auto& [sid, s] : c.streams)
        if (s.local && !s.delivered && s.on_response) s.on_response(std::nullopt);
    for (auto& p : c.pending)
        if (p.fn) p.fn(std::nullopt);
    for (auto& k : c.cids) cid_map.erase(k);
    auto cb_closed = cb.on_closed;
    conns.erase(it);
    if (cb_closed) cb_closed(id, reason);
}

void Endpoint::Impl::write_conn(Conn& c)
{
    if (!c.conn) return;
    uint8_t buf[1500];
    size_t destlen = std::min<size_t>(sizeof(buf), ngtcp2_conn_get_max_tx_udp_payload_size(c.conn));
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;
    uint64_t ts = now_ns();
    std::vector<int64_t> blocked;
    for (int guard = 0; guard < 100000; guard++) {
        Stream* s = nullptr;
        for (auto& [sid, st] : c.streams) {
            if (std::find(blocked.begin(), blocked.end(), sid) != blocked.end()) continue;
            if (st.sent < st.send_buf.size() || (st.fin_pending && !st.fin_sent)) {
                s = &st;
                break;
            }
        }
        ngtcp2_vec v{};
        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        int64_t sid = -1;
        size_t nv = 0;
        if (s) {
            sid = s->id;
            v.base = s->send_buf.data() + s->sent;
            v.len = s->send_buf.size() - s->sent;
            nv = 1;
            if (s->fin_pending) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        }
        ngtcp2_ssize ndatalen = -1;
        auto n = ngtcp2_conn_writev_stream(c.conn, &ps.path, &pi, buf, destlen, &ndatalen, flags, sid, nv ? &v : nullptr, nv, ts);
        if (n < 0) {
            if (n == NGTCP2_ERR_WRITE_MORE) {
                if (s && ndatalen >= 0) {
                    s->sent += size_t(ndatalen);
                    if (s->fin_pending && s->sent == s->send_buf.size()) s->fin_sent = true;
                }
                continue;
            }
            if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED || n == NGTCP2_ERR_STREAM_SHUT_WR || n == NGTCP2_ERR_STREAM_NOT_FOUND) {
                if (s) blocked.push_back(s->id);
                continue;
            }
            ngtcp2_ccerr_set_liberr(&c.ccerr, int(n), nullptr, 0);
            c.dead = true;
            return;
        }
        if (s && ndatalen >= 0) {
            s->sent += size_t(ndatalen);
            if (s->fin_pending && s->sent == s->send_buf.size()) s->fin_sent = true;
        }
        if (n == 0) break;
        sendto(sock, (const char*)buf, size_t(n), 0, (sockaddr*)&c.remote, c.remote_len);
    }
    ngtcp2_conn_update_pkt_tx_time(c.conn, ts);
}

void Endpoint::Impl::on_packet(const uint8_t* data, size_t n, const sockaddr* from, socklen_t fromlen)
{
    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(&vc, data, n, SCID_LEN);
    if (rv != 0) return; // includes NGTCP2_ERR_VERSION_NEGOTIATION: unsupported version, ignore
    Conn* c = nullptr;
    auto it = cid_map.find(cid_key(vc.dcid, vc.dcidlen));
    if (it != cid_map.end()) {
        auto ci = conns.find(it->second);
        if (ci != conns.end()) c = ci->second.get();
    }
    if (!c) {
        if (!opt.accept_inbound) return;
        ngtcp2_pkt_hd hd;
        if (ngtcp2_accept(&hd, data, n) != 0) return;
        c = new_conn(true, from, fromlen, &hd, "", next_id++);
        if (!c) return;
    }
    ngtcp2_path path = c->path();
    path.remote.addr = (ngtcp2_sockaddr*)from;
    path.remote.addrlen = fromlen;
    ngtcp2_pkt_info pi{};
    rv = ngtcp2_conn_read_pkt(c->conn, &path, &pi, data, n, now_ns());
    if (rv != 0) {
        if (rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_CLOSING) {
            drop(c->id, "closed by peer", false);
            return;
        }
        if (rv == NGTCP2_ERR_DROP_CONN) {
            drop(c->id, "dropped", false);
            return;
        }
        if (rv == NGTCP2_ERR_CRYPTO) ngtcp2_ccerr_set_tls_alert(&c->ccerr, ngtcp2_conn_get_tls_alert(c->conn), nullptr, 0);
        else ngtcp2_ccerr_set_liberr(&c->ccerr, rv, nullptr, 0);
        drop(c->id, std::string("read_pkt: ") + ngtcp2_strerror(rv), true);
        return;
    }
    write_conn(*c);
    if (c->dead) drop(c->id, "write error", true);
}

void Endpoint::Impl::loop()
{
    std::vector<uint8_t> buf(65536);
    while (running) {
        // commands
        std::deque<std::function<void()>> q;
        {
            std::lock_guard<std::mutex> l(mu);
            q.swap(cmds);
        }
        for (auto& f : q) f();
        // timers
        uint64_t now = now_ns();
        uint64_t next = now + 200ull * 1000000; // at most 200 ms
        std::vector<ConnId> ids;
        for (auto& [id, c] : conns) ids.push_back(id);
        for (ConnId id : ids) {
            auto ci = conns.find(id);
            if (ci == conns.end()) continue;
            Conn& c = *ci->second;
            // response timeouts
            for (auto& [sid, s] : c.streams)
                if (s.local && !s.delivered && s.deadline && now >= s.deadline) {
                    s.delivered = true;
                    ngtcp2_conn_shutdown_stream(c.conn, 0, sid, 1);
                    if (s.on_response) s.on_response(std::nullopt);
                }
            uint64_t exp = ngtcp2_conn_get_expiry(c.conn);
            if (exp <= now) {
                int rv = ngtcp2_conn_handle_expiry(c.conn, now);
                if (rv != 0) {
                    drop(id, rv == NGTCP2_ERR_IDLE_CLOSE ? "idle timeout" : std::string("expiry: ") + ngtcp2_strerror(rv), rv != NGTCP2_ERR_IDLE_CLOSE);
                    continue;
                }
            }
            write_conn(c);
            if (c.dead) {
                drop(id, "write error", true);
                continue;
            }
            exp = ngtcp2_conn_get_expiry(c.conn);
            if (exp < next) next = exp;
        }
        int ms = next > now ? int((next - now) / 1000000) : 0;
        pollfd_t fds[2];
        fds[0].fd = sock;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = wake_rx;
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        poll_fds(fds, 2, ms);
        if (fds[1].revents & POLLIN) {
            char tmp[64];
            while (recv(wake_rx, tmp, sizeof(tmp), 0) > 0) {
            }
        }
        if (fds[0].revents & POLLIN) {
            for (int i = 0; i < 256; i++) {
                sockaddr_storage from{};
                socklen_t fl = sizeof(from);
                auto r = recvfrom(sock, (char*)buf.data(), buf.size(), 0, (sockaddr*)&from, &fl);
                if (r <= 0) break;
                on_packet(buf.data(), size_t(r), (sockaddr*)&from, fl);
            }
        }
    }
    std::vector<ConnId> ids;
    for (auto& [id, c] : conns) ids.push_back(id);
    for (ConnId id : ids) drop(id, "shutdown", true);
}

ConnId Endpoint::connect(const SockAddr& remote, const std::string& sni)
{
    ConnId id;
    {
        std::lock_guard<std::mutex> l(m->mu);
        id = m->next_id++;
    }
    m->post([this, remote, sni, id] {
        sockaddr_storage ss;
        socklen_t len;
        if (!to_sockaddr(remote, ss, len) || ss.ss_family != m->local_ss.ss_family) {
            if (m->cb.on_closed) m->cb.on_closed(id, "address family mismatch");
            return;
        }
        Conn* c = m->new_conn(false, (sockaddr*)&ss, len, nullptr, sni, id);
        if (!c) {
            if (m->cb.on_closed) m->cb.on_closed(id, "connection setup failed");
            return;
        }
        m->write_conn(*c);
    });
    return id;
}

void Endpoint::send(ConnId cid, Bytes payload, size_t limit, std::chrono::milliseconds timeout, ResponseFn fn)
{
    m->post([this, cid, p = std::move(payload), limit, timeout, fn = std::move(fn)]() mutable {
        auto it = m->conns.find(cid);
        if (it == m->conns.end()) {
            if (fn) fn(std::nullopt);
            return;
        }
        Conn& c = *it->second;
        c.pending.push_back(PendingSend{std::move(p), limit, timeout, std::move(fn)});
        if (c.handshake_done) c.open_pending();
        m->write_conn(c);
    });
}

void Endpoint::respond(ConnId cid, int64_t stream, Bytes payload)
{
    m->post([this, cid, stream, p = std::move(payload)]() mutable {
        auto it = m->conns.find(cid);
        if (it == m->conns.end()) return;
        auto si = it->second->streams.find(stream);
        if (si == it->second->streams.end()) return;
        si->second.send_buf = std::move(p);
        si->second.sent = 0;
        si->second.fin_pending = true;
        m->write_conn(*it->second);
    });
}

void Endpoint::finish(ConnId cid, int64_t stream) { respond(cid, stream, {}); }

void Endpoint::close(ConnId cid, const std::string& reason)
{
    m->post([this, cid, reason] { m->drop(cid, reason, true); });
}

} // namespace bt::quic
