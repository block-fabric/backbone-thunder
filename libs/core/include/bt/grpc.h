// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Small gRPC client (HTTP/2 via nghttp2, TLS via OpenSSL with ALPN h2, or h2c for
// http:// URLs), for talking to bip300301_enforcer's cusf.mainchain.v1 services or to
// backboned's implementation of them. Unary and server-streaming calls.
#pragma once

#include <bt/bytes.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace bt::grpc {

struct Status {
    int code = 0; // grpc-status; -1 = transport error
    std::string message;
    bool ok() const { return code == 0; }
    static constexpr int NOT_FOUND = 5;
    static constexpr int UNIMPLEMENTED = 12;
};

struct Url {
    bool tls = false;
    std::string host;
    uint16_t port = 0;
    std::string path_prefix; // without trailing '/'
    static std::optional<Url> parse(const std::string& s);
};

class Channel {
public:
    /** url: http://host:port[/prefix] (h2c) or https://host:port[/prefix] (TLS, ALPN h2).
     *  ca_file: PEM bundle for TLS (empty: system default locations). */
    explicit Channel(const std::string& url, std::string ca_file = "");
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /** method: "/package.Service/Method" */
    Status call(const std::string& method, const Bytes& request, Bytes& response, int timeout_ms = 120000);
    /** Server-streaming call; on_message returning false cancels. Returns when the stream ends. */
    Status stream(const std::string& method, const Bytes& request, const std::function<bool(const Bytes&)>& on_message,
                  const std::atomic<bool>* cancel = nullptr);
    const std::string& url() const { return url_; }

    struct Conn;

private:
    std::string url_, ca_;
    std::mutex mu_;
    std::unique_ptr<Conn> conn_;
    Status run(const std::string& method, const Bytes& request, const std::function<bool(const Bytes&)>& on_message, int timeout_ms,
               const std::atomic<bool>* cancel);
};

} // namespace bt::grpc
