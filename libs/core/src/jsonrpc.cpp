// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/jsonrpc.h>
#include <bt/parse.h>

#include <algorithm>

#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static void sclose(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static void sclose(sock_t s) { ::close(s); }
#endif

namespace bt::jsonrpc {

static const UniValue NULLV;

const UniValue& Params::operator[](size_t i) const { return i < v.size() ? v[i] : NULLV; }

void Server::add(const std::string& method, std::vector<std::string> names, size_t required, Handler h)
{
    methods_[method] = Method{std::move(names), required, std::move(h)};
}

std::vector<std::string> Server::methods() const
{
    std::vector<std::string> r;
    for (auto& [k, _] : methods_) r.push_back(k);
    return r;
}

static UniValue error_obj(int code, const std::string& msg)
{
    UniValue e(UniValue::VOBJ);
    e.pushKV("code", code);
    e.pushKV("message", msg);
    return e;
}

static UniValue response(const UniValue& id, const UniValue* result, const UniValue* error)
{
    UniValue r(UniValue::VOBJ);
    r.pushKV("jsonrpc", "2.0");
    if (result) r.pushKV("result", *result);
    if (error) r.pushKV("error", *error);
    r.pushKV("id", id);
    return r;
}

UniValue Server::call(const UniValue& req)
{
    UniValue id = req["id"];
    if (!req.isObject() || !req["method"].isStr()) {
        auto e = error_obj(-32600, "Invalid request");
        return response(id, nullptr, &e);
    }
    auto it = methods_.find(req["method"].get_str());
    if (it == methods_.end()) {
        auto e = error_obj(-32601, "Method not found");
        return response(id, nullptr, &e);
    }
    Params p;
    const UniValue& params = req["params"];
    auto& m = it->second;
    if (params.isArray()) {
        if (params.size() > m.names.size()) {
            auto e = error_obj(-32602, "Invalid params");
            return response(id, nullptr, &e);
        }
        p.v = params.getValues();
    } else if (params.isObject()) {
        for (auto& n : m.names) p.v.push_back(params[n]);
    } else if (!params.isNull()) {
        auto e = error_obj(-32602, "Invalid params");
        return response(id, nullptr, &e);
    }
    size_t present = 0;
    for (size_t i = 0; i < m.required; i++)
        if (i < p.v.size() && !p.v[i].isNull()) present++;
    if (present < m.required) {
        auto e = error_obj(-32602, "Invalid params");
        return response(id, nullptr, &e);
    }
    try {
        UniValue r = m.h(p);
        return response(id, &r, nullptr);
    } catch (const RpcError& ex) {
        auto e = error_obj(ex.code, ex.what());
        return response(id, nullptr, &e);
    } catch (const std::exception& ex) {
        auto e = error_obj(-1, ex.what());
        return response(id, nullptr, &e);
    }
}

std::string Server::handle(const std::string& body)
{
    UniValue req;
    if (!req.read(body)) {
        auto e = error_obj(-32700, "Parse error");
        return response(UniValue(), nullptr, &e).write();
    }
    if (req.isArray()) {
        UniValue out(UniValue::VARR);
        for (auto& r : req.getValues()) {
            bool notification = r.isObject() && !r.exists("id");
            UniValue resp = call(r);
            if (!notification) out.push_back(resp);
        }
        return out.empty() ? "" : out.write();
    }
    bool notification = req.isObject() && !req.exists("id");
    UniValue resp = call(req);
    return notification ? "" : resp.write();
}

static bool is_loopback_ip(const std::string& ip) { return ip.rfind("127.", 0) == 0 || ip == "::1" || ip == "localhost"; }

bool Server::listen(const std::string& ip, uint16_t port, std::string& err)
{
    if (!is_loopback_ip(ip) && !options.allow_external) {
        err = "refusing to serve the unauthenticated RPC on non-loopback " + ip + " (pass --bt-rpc-allow-external to override)";
        return false;
    }
    bind_ip_ = ip;
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) {
        err = "bad RPC address " + ip;
        sclose(fd);
        return false;
    }
    if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(fd, 64) != 0) {
        err = "cannot bind RPC " + ip + ":" + std::to_string(port);
        sclose(fd);
        return false;
    }
    fd_ = intptr_t(fd);
    running_ = true;
    thr_ = std::thread([this] {
        while (running_) {
            sock_t c = accept(sock_t(fd_), nullptr, nullptr);
            if (c == sock_t(-1)) {
                if (!running_) break;
                continue;
            }
            std::thread([this, c] { serve(intptr_t(c)); }).detach();
        }
    });
    return true;
}

void Server::stop()
{
    if (!running_.exchange(false)) return;
#ifdef _WIN32
    shutdown(sock_t(fd_), SD_BOTH);
#else
    shutdown(sock_t(fd_), SHUT_RDWR);
#endif
    sclose(sock_t(fd_));
    if (thr_.joinable()) thr_.join();
}

static std::string lower(std::string v)
{
    for (auto& ch : v) ch = char(std::tolower((unsigned char)ch));
    return v;
}

// Reads one request; headers keys lower-cased. Returns false on malformed input.
static bool read_request(sock_t c, std::string& method, std::map<std::string, std::string>& headers, std::string& body, bool& keep_alive,
                         size_t max_body)
{
    std::string buf;
    char tmp[8192];
    size_t hdr_end;
    while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
        auto n = recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, size_t(n));
        if (buf.size() > 64 * 1024) return false;
    }
    std::string head = buf.substr(0, hdr_end);
    std::istringstream hs(head);
    std::string line;
    std::getline(hs, line);
    method = line.substr(0, line.find(' '));
    keep_alive = line.find("HTTP/1.1") != std::string::npos;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = lower(line.substr(0, colon)), v = line.substr(colon + 1);
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
        headers[k] = v;
    }
    if (auto it = headers.find("connection"); it != headers.end()) keep_alive = lower(it->second) != "close";
    size_t clen = 0;
    if (auto it = headers.find("content-length"); it != headers.end()) {
        auto v = parse_u64(it->second);
        if (!v || *v > max_body) return false;
        clen = size_t(*v);
    }
    body = buf.substr(hdr_end + 4);
    while (body.size() < clen) {
        auto n = recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        body.append(tmp, size_t(n));
    }
    body.resize(clen);
    return true;
}

static void send_all(sock_t c, const std::string& s)
{
    size_t off = 0;
    while (off < s.size()) {
        auto n = send(c, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += size_t(n);
    }
}

static std::string host_name(const std::string& host)
{
    if (!host.empty() && host[0] == '[') {
        auto e = host.find(']');
        return e == std::string::npos ? host : host.substr(1, e - 1);
    }
    auto c = host.rfind(':');
    return lower(c == std::string::npos ? host : host.substr(0, c));
}

static std::string status_line(int code)
{
    switch (code) {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 403: return "HTTP/1.1 403 Forbidden\r\n";
    case 405: return "HTTP/1.1 405 Method Not Allowed\r\n";
    case 415: return "HTTP/1.1 415 Unsupported Media Type\r\n";
    default: return "HTTP/1.1 400 Bad Request\r\n";
    }
}

std::string Server::http(const std::string& method, const std::map<std::string, std::string>& headers, const std::string& body)
{
    auto reply = [](int code, const std::string& extra, const std::string& content, const std::string& ctype) {
        std::string r = status_line(code) + extra;
        if (!ctype.empty()) r += "Content-Type: " + ctype + "\r\n";
        r += "Content-Length: " + std::to_string(content.size()) + "\r\n\r\n" + content;
        return r;
    };
    // DNS rebinding: the Host must be ours
    if (auto it = headers.find("host"); it != headers.end()) {
        std::string h = host_name(it->second);
        bool ok = h == "localhost" || h == "127.0.0.1" || h == "::1" || h == lower(bind_ip_) ||
                  std::find(options.allowed_hosts.begin(), options.allowed_hosts.end(), h) != options.allowed_hosts.end();
        if (!ok) return reply(403, "", "", "");
    }
    // CSRF: browsers send Origin; only explicitly allowed origins get through
    std::string cors;
    if (auto it = headers.find("origin"); it != headers.end()) {
        const std::string& o = it->second;
        bool any = std::find(options.allowed_origins.begin(), options.allowed_origins.end(), "*") != options.allowed_origins.end();
        bool listed = std::find(options.allowed_origins.begin(), options.allowed_origins.end(), o) != options.allowed_origins.end();
        if (!any && !listed) return reply(403, "", "", "");
        cors = "Access-Control-Allow-Origin: " + (any ? std::string("*") : o) +
               "\r\nAccess-Control-Allow-Methods: POST, OPTIONS\r\nAccess-Control-Allow-Headers: content-type\r\nVary: Origin\r\n";
    }
    if (method == "OPTIONS") return reply(cors.empty() ? 403 : 200, cors, "", "");
    if (method != "POST") return reply(405, cors, "", "");
    auto ct = headers.find("content-type");
    if (ct == headers.end() || lower(ct->second).rfind("application/json", 0) != 0) return reply(415, cors, "", "");
    std::string out = handle(body);
    return reply(200, cors, out, "application/json; charset=utf-8");
}

void Server::serve(intptr_t ci)
{
    sock_t c = sock_t(ci);
    try {
        for (;;) {
            std::string method, body;
            std::map<std::string, std::string> headers;
            bool keep;
            if (!read_request(c, method, headers, body, keep, options.max_body)) {
                send_all(c, status_line(400) + "Content-Length: 0\r\nConnection: close\r\n\r\n");
                break;
            }
            send_all(c, http(method, headers, body));
            if (!keep) break;
        }
    } catch (...) {
        // never let a malformed request take the process down
    }
    sclose(c);
}

UniValue call(const std::string& host, uint16_t port, const std::string& method, const UniValue& params, std::string& err)
{
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        err = "cannot resolve " + host;
        return {};
    }
    sock_t c = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(c, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        sclose(c);
        err = "cannot connect to " + host + ":" + std::to_string(port);
        return {};
    }
    freeaddrinfo(res);
    UniValue req(UniValue::VOBJ);
    req.pushKV("jsonrpc", "2.0");
    req.pushKV("id", 1);
    req.pushKV("method", method);
    req.pushKV("params", params);
    std::string body = req.write();
    send_all(c, "POST / HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    std::string resp;
    char tmp[8192];
    for (;;) {
        auto n = recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        resp.append(tmp, size_t(n));
    }
    sclose(c);
    auto p = resp.find("\r\n\r\n");
    if (p == std::string::npos) {
        err = "bad HTTP response";
        return {};
    }
    UniValue v;
    if (!v.read(resp.substr(p + 4))) {
        err = "bad JSON response";
        return {};
    }
    return v;
}

} // namespace bt::jsonrpc
