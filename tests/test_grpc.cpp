// gRPC client retry rules: non-idempotent calls are never re-sent.
#include "test.h"

#include <bt/grpc.h>

#include <atomic>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
// A TCP server that reads whatever a client sends (preface + request) and then closes
// without answering: the request has reached the server but no response comes back.
struct BlackHole {
    int fd = -1;
    uint16_t port = 0;
    std::atomic<int> accepted{0};
    std::atomic<bool> stop{false};
    std::thread thr;
    BlackHole()
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, (sockaddr*)&a, sizeof(a));
        socklen_t l = sizeof(a);
        getsockname(fd, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        listen(fd, 8);
        thr = std::thread([this] {
            while (!stop) {
                int c = accept(fd, nullptr, nullptr);
                if (c < 0) return;
                accepted++;
                char buf[4096];
                // read until the client has sent a HEADERS frame's worth of bytes
                ssize_t total = 0;
                for (int i = 0; i < 20 && total < 60; i++) {
                    ssize_t n = recv(c, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    total += n;
                }
                close(c);
            }
        });
    }
    ~BlackHole()
    {
        stop = true;
        shutdown(fd, SHUT_RDWR);
        close(fd);
        thr.join();
    }
};
} // namespace

TEST(grpc_non_idempotent_not_retried)
{
    BlackHole srv;
    bt::grpc::Channel ch("http://127.0.0.1:" + std::to_string(srv.port));
    bt::Bytes resp;
    auto s = ch.call("/cusf.mainchain.v1.WalletService/CreateDepositTransaction", {}, resp, 5000, false);
    CHECK(!s.ok());
    CHECK_EQ(ch.requests_sent(), uint64_t(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(srv.accepted.load(), 1);
}

TEST(grpc_idempotent_retried_once)
{
    BlackHole srv;
    bt::grpc::Channel ch("http://127.0.0.1:" + std::to_string(srv.port));
    bt::Bytes resp;
    auto s = ch.call("/cusf.mainchain.v1.ValidatorService/GetChainTip", {}, resp, 5000, true);
    CHECK(!s.ok());
    CHECK_EQ(ch.requests_sent(), uint64_t(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(srv.accepted.load(), 2);
}

TEST(grpc_connect_failure_sends_nothing)
{
    bt::grpc::Channel ch("http://127.0.0.1:1"); // nothing listens on port 1
    bt::Bytes resp;
    auto s = ch.call("/cusf.mainchain.v1.WalletService/CreateNewAddress", {}, resp, 2000, false);
    CHECK(!s.ok());
    CHECK_EQ(ch.requests_sent(), uint64_t(0));
}
