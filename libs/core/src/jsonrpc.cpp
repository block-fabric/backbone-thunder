// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/jsonrpc.h>

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

bool Server::listen(const std::string& ip, uint16_t port, std::string& err)
{
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

static bool read_request(sock_t c, std::string& method, std::string& body, bool& keep_alive)
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
    size_t clen = 0;
    keep_alive = line.find("HTTP/1.1") != std::string::npos;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon), v = line.substr(colon + 1);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        for (auto& ch : k) ch = char(std::tolower((unsigned char)ch));
        if (k == "content-length") clen = size_t(std::stoull(v));
        if (k == "connection") {
            for (auto& ch : v) ch = char(std::tolower((unsigned char)ch));
            keep_alive = v != "close";
        }
    }
    if (clen > 32 * 1024 * 1024) return false; // jsonrpsee default max request body 10 MB; blocks may be larger
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

void Server::serve(intptr_t ci)
{
    sock_t c = sock_t(ci);
    for (;;) {
        std::string method, body;
        bool keep;
        if (!read_request(c, method, body, keep)) break;
        std::string cors = "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: *\r\nAccess-Control-Allow-Headers: *\r\n";
        if (method == "OPTIONS") {
            send_all(c, "HTTP/1.1 200 OK\r\n" + cors + "Content-Length: 0\r\n\r\n");
        } else if (method != "POST") {
            send_all(c, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n");
        } else {
            std::string out = handle(body);
            send_all(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n" + cors + "Content-Length: " + std::to_string(out.size()) +
                            "\r\n\r\n" + out);
        }
        if (!keep) break;
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
