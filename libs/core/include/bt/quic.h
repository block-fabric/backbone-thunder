// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// QUIC endpoint for the LayerTwo-family P2P protocol, on ngtcp2 (MIT) with OpenSSL 3.5's
// QUIC TLS API (Apache-2.0). Their nodes run quinn 0.11 + rustls 0.23; we interoperate
// on the exact conventions they use (lib/net/mod.rs, lib/net/peer/mod.rs):
//  * QUIC v1, TLS 1.3, no ALPN at all (rustls rejects an offered-but-unmatched ALPN);
//  * the server presents a fresh self-signed certificate (ECDSA P-256, like rcgen's
//    default) for "localhost" + --server-name values; clients do not verify it;
//  * every message is its own bidirectional stream: magic(4) || bincode(msg), FIN; a
//    response, if any, comes back on the same stream, FIN-terminated.
// One endpoint (one UDP socket) serves inbound connections and dials outbound ones.
#pragma once

#include <bt/bytes.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bt::quic {

struct SockAddr {
    std::string ip; // numeric
    uint16_t port = 0;
    std::string to_string() const;
    bool operator==(const SockAddr&) const = default;
};

/** Resolve host:port to numeric addresses (IPv4 first). */
std::vector<SockAddr> resolve(const std::string& host, uint16_t port);

using ConnId = uint64_t;

struct Callbacks {
    /** Handshake completed (inbound or outbound). */
    std::function<void(ConnId, const SockAddr& remote, bool inbound)> on_connected;
    /** A peer-initiated bidi stream finished (FIN) with this payload; respond via
     *  Endpoint::respond(conn, stream, bytes) or Endpoint::finish(conn, stream). */
    std::function<void(ConnId, int64_t stream, Bytes payload)> on_request;
    /** Connection gone (error string empty on clean close). */
    std::function<void(ConnId, const std::string& reason)> on_closed;
};

struct Options {
    /** Extra DNS names for the self-signed certificate ("localhost" is always included). */
    std::vector<std::string> server_names;
    size_t max_request_bytes = 100 * 1024; // READ_REQUEST_LIMIT for peer-initiated streams
    std::chrono::milliseconds idle_timeout{30000};
    bool accept_inbound = true;
};

/** Response to a locally-initiated stream: payload, or nullopt on reset/timeout/limit. */
using ResponseFn = std::function<void(std::optional<Bytes>)>;

class Endpoint {
public:
    Endpoint(Callbacks cb, Options opt);
    ~Endpoint();
    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    /** Bind the UDP socket ("0.0.0.0", 4009) and start the I/O thread. */
    bool start(const SockAddr& bind, std::string& err);
    void stop();
    SockAddr local_address() const;

    /** Dial a peer; on_connected fires after the handshake. sni = domain or "localhost". */
    ConnId connect(const SockAddr& remote, const std::string& sni);
    /** Open a bidi stream, send payload + FIN. If response_limit > 0 the FIN-terminated
     *  response is delivered to on_response (nullopt on error/limit/timeout). */
    void send(ConnId c, Bytes payload, size_t response_limit, std::chrono::milliseconds timeout, ResponseFn on_response);
    /** Write a response on a peer-initiated stream and FIN it. */
    void respond(ConnId c, int64_t stream, Bytes payload);
    /** FIN a peer-initiated stream without payload (requests that take no response). */
    void finish(ConnId c, int64_t stream);
    void close(ConnId c, const std::string& reason);
    /** DER of our certificate. */
    Bytes certificate_der() const;

    struct Impl; // implementation detail (public so file-local helpers can name it)

private:
    std::unique_ptr<Impl> m;
};

} // namespace bt::quic
