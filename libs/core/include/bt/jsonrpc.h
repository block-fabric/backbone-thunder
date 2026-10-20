// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// JSON-RPC 2.0 over HTTP/1.1, shaped like jsonrpsee's server (what the LayerTwo nodes
// use): POST with a single or batch request, positional or named params, -32601 for
// unknown methods, -32602 for bad params, -32700 for parse errors, application errors
// with code -1. CORS is permissive as in theirs (tower_http CorsLayer::permissive).
#pragma once

#include <univalue.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bt::jsonrpc {

struct RpcError : std::runtime_error {
    int code;
    RpcError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};
/** Application error, as their custom_err (code -1). */
inline RpcError app_error(const std::string& m) { return RpcError(-1, m); }
inline RpcError invalid_params(const std::string& m) { return RpcError(-32602, "Invalid params"); }

/** Parameters normalised to positional order. Missing optional params are null. */
struct Params {
    std::vector<UniValue> v;
    const UniValue& operator[](size_t i) const;
    size_t size() const { return v.size(); }
};

using Handler = std::function<UniValue(const Params&)>;

class Server {
public:
    /** names: parameter names in order (for named-param calls); required: how many must be present. */
    void add(const std::string& method, std::vector<std::string> names, size_t required, Handler h);
    bool has(const std::string& method) const { return methods_.count(method) > 0; }
    std::vector<std::string> methods() const;
    /** Process one HTTP body; returns the response body ("" for notifications only). */
    std::string handle(const std::string& body);

    bool listen(const std::string& ip, uint16_t port, std::string& err);
    void stop();
    ~Server() { stop(); }

private:
    struct Method {
        std::vector<std::string> names;
        size_t required;
        Handler h;
    };
    std::map<std::string, Method> methods_;
    UniValue call(const UniValue& req);
    std::atomic<bool> running_{false};
    intptr_t fd_ = -1;
    std::thread thr_;
    void serve(intptr_t c);
};

/** Minimal blocking JSON-RPC client (for <chain>-cli and tests). */
UniValue call(const std::string& host, uint16_t port, const std::string& method, const UniValue& params, std::string& err);

} // namespace bt::jsonrpc
