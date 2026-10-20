// Hardening tests: RPC HTTP policy, non-throwing parsers.
#include "test.h"

#include <bt/jsonrpc.h>
#include <bt/parse.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace bt;

static void add_ping(jsonrpc::Server& s) { s.add("ping", {}, 0, [](const jsonrpc::Params&) { return UniValue("pong"); }); }

static int status_of(const std::string& resp) { return std::atoi(resp.substr(9, 3).c_str()); }

TEST(rpc_rejects_foreign_origin_and_hosts)
{
    jsonrpc::Server s;
    add_ping(s);
    std::string body = R"({"jsonrpc":"2.0","id":1,"method":"ping","params":[]})";
    std::map<std::string, std::string> ok{{"host", "127.0.0.1:6009"}, {"content-type", "application/json"}};
    auto r = s.http("POST", ok, body);
    CHECK_EQ(status_of(r), 200);
    CHECK(r.find("pong") != std::string::npos);
    CHECK(r.find("Access-Control-Allow-Origin") == std::string::npos); // no CORS by default
    auto evil = ok;
    evil["origin"] = "https://evil.example";
    CHECK_EQ(status_of(s.http("POST", evil, body)), 403);
    CHECK_EQ(status_of(s.http("OPTIONS", evil, "")), 403);
    auto rebind = ok;
    rebind["host"] = "attacker.example:6009";
    CHECK_EQ(status_of(s.http("POST", rebind, body)), 403);
    auto plain = ok;
    plain["content-type"] = "text/plain";
    CHECK_EQ(status_of(s.http("POST", plain, body)), 415);
    s.options.allowed_origins = {"http://localhost:3000"};
    auto allowed = ok;
    allowed["origin"] = "http://localhost:3000";
    auto ra = s.http("POST", allowed, body);
    CHECK_EQ(status_of(ra), 200);
    CHECK(ra.find("Access-Control-Allow-Origin: http://localhost:3000") != std::string::npos);
    CHECK_EQ(status_of(s.http("OPTIONS", allowed, "")), 200);
}

TEST(rpc_refuses_external_bind_by_default)
{
    jsonrpc::Server s;
    add_ping(s);
    std::string err;
    CHECK(!s.listen("0.0.0.0", 49141, err));
    CHECK(err.find("non-loopback") != std::string::npos);
}

TEST(rpc_bad_content_length_does_not_crash)
{
    jsonrpc::Server s;
    add_ping(s);
    std::string err;
    REQUIRE(s.listen("127.0.0.1", 49142, err));
    for (const char* cl : {"abc", "-1", "99999999999999999999999", "18446744073709551616"}) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(49142);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(connect(fd, (sockaddr*)&a, sizeof(a)) == 0);
        std::string req = std::string("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: ") + cl + "\r\n\r\n{}";
        send(fd, req.data(), req.size(), 0);
        char buf[256] = {0};
        recv(fd, buf, sizeof(buf) - 1, 0);
        CHECK(std::string(buf).rfind("HTTP/1.1 400", 0) == 0);
        close(fd);
    }
    // still serving
    UniValue r = jsonrpc::call("127.0.0.1", 49142, "ping", UniValue(UniValue::VARR), err);
    CHECK(err.empty());
    CHECK_EQ(r["result"].getValStr(), std::string("pong"));
    s.stop();
}

TEST(parse_ports_and_hostports)
{
    CHECK(parse_port("4009") == std::optional<uint16_t>(4009));
    CHECK(!parse_port("0").has_value());
    CHECK(parse_port("0", true) == std::optional<uint16_t>(0));
    CHECK(!parse_port("65536").has_value());
    CHECK(!parse_port("-1").has_value());
    CHECK(!parse_port("40 09").has_value());
    CHECK(!parse_port("99999999999999999999999999").has_value());
    CHECK(!parse_port("").has_value());
    auto a = split_hostport("seed.example:4009");
    CHECK(a && a->first == "seed.example" && a->second == 4009);
    auto b = split_hostport("[::1]:6009");
    CHECK(b && b->first == "::1" && b->second == 6009);
    CHECK(split_hostport("host", uint16_t(4009))->second == 4009);
    CHECK(!split_hostport("host:").has_value());
    CHECK(!split_hostport("host:abc").has_value());
    CHECK(!split_hostport(":4009").has_value());
    CHECK(!split_hostport("[::1").has_value());
    CHECK(!parse_u64("18446744073709551616").has_value());
    CHECK(parse_u64("18446744073709551615") == std::optional<uint64_t>(UINT64_MAX));
}
