// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// `thunder`: drop-in replacement for thunder-rust's `thunder_app` on eCash slot 9.
// Accepts the same command-line flags (app/cli.rs), the same defaults (P2P 0.0.0.0:4009,
// RPC 127.0.0.1:6009, datadir <data_dir>/thunder, logs <datadir>/logs/v<version>), and
// serves the same JSON-RPC methods. There is no GUI: without --headless it runs headless.
// Our own options use the --bt- prefix so they can never collide with theirs. Unknown
// flags are reported and ignored (the orchestrator stops a daemon that fails to parse).
#include <thunder/app.h>

#include <bt/frost.h>
#include <bt/hd.h>
#include <bt/jsonrpc.h>
#include <bt/log.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace thunder;

static const char* VERSION = "0.18.2";
static const char* OUR_VERSION = "backbone-thunder 0.1.0 (thunder 0.18.2 compatible)";

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

static std::string default_datadir()
{
#ifdef _WIN32
    const char* app = std::getenv("APPDATA");
    return std::string(app ? app : ".") + "\\thunder";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/thunder";
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return std::string(x) + "/thunder";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/thunder";
#endif
}

static bool parse_sockaddr(const std::string& s, bt::quic::SockAddr& out)
{
    std::string host;
    std::string port;
    if (!s.empty() && s[0] == '[') {
        auto e = s.find(']');
        if (e == std::string::npos || e + 1 >= s.size() || s[e + 1] != ':') return false;
        host = s.substr(1, e - 1);
        port = s.substr(e + 2);
    } else {
        auto c = s.rfind(':');
        if (c == std::string::npos) return false;
        host = s.substr(0, c);
        port = s.substr(c + 1);
    }
    try {
        int p = std::stoi(port);
        if (p < 0 || p > 65535) return false;
        out = {host, uint16_t(p)};
    } catch (...) {
        return false;
    }
    return true;
}

static void usage()
{
    std::printf(
        "Usage: thunder [OPTIONS]\n\n"
        "Options (thunder-rust 0.18.2 compatible):\n"
        "      --add-peer <ADD_PEERS>                 Additional peers to dial on startup, as host:port\n"
        "  -d, --datadir <DATADIR>                    Data directory [default: %s]\n"
        "      --headless                             Accepted; there is no GUI\n"
        "      --log-dir <LOG_DIR>                    [default: <DATADIR>/logs/v%s]; empty disables file logs\n"
        "      --log-level-file <LOG_LEVEL_FILE>      [default: WARN]\n"
        "      --log-level <LOG_LEVEL>                [default: DEBUG]\n"
        "      --mainchain-grpc-url <URL>             [default: http://localhost:50051]\n"
        "      --mainchain-grpc-ca-cert <PEM>         Extra CA for an https mainchain URL\n"
        "      --mnemonic-seed-phrase-path <PATH>     Path to a mnemonic seed phrase\n"
        "  -n, --net-addr <NET_ADDR>                  [default: 0.0.0.0:4009]\n"
        "      --network <NETWORK>                    betanet | forknet | regtest | signet [default: betanet]\n"
        "      --network-magic <HEX>                  Override the 4-byte network magic\n"
        "  -p, --private-rpc-addr <ADDR>              [default: 127.0.0.1:6009]\n"
        "  -r, --rpc-addr <ADDR>                      [default: 127.0.0.1:6009]\n"
        "      --server-name <NAME>                   Host name for the P2P certificate (repeatable)\n"
        "      --wallet-dir <DIR>                     [default: <DATADIR>]\n"
        "  -h, --help / -V, --version\n\n"
        "Backbone options:\n"
        "      --bt-no-seeds                          Do not dial the built-in seed peers\n"
        "      --bt-no-p2p                            Disable P2P networking\n"
        "      --bt-wallet-derivation <v0.18.1|v0.18.0>  Key derivation of an imported mnemonic [default: v0.18.1]\n",
        default_datadir().c_str(), VERSION);
}

int main(int argc, char** argv)
{
    NodeConfig cfg;
    cfg.datadir = default_datadir();
    std::optional<std::string> log_dir, wallet_dir, mnemonic_path;
    bt::log::Level log_level = bt::log::Level::Debug, log_level_file = bt::log::Level::Warn;
    bt::quic::SockAddr rpc_addr{"127.0.0.1", net::DEFAULT_RPC_PORT}, private_rpc_addr = rpc_addr;
    bool headless = false;
    bt::hd::Variant variant = bt::hd::Variant::V0_18_1;
    std::vector<std::string> unknown;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i], val;
        bool has_eq = false;
        if (a.rfind("--", 0) == 0) {
            auto eq = a.find('=');
            if (eq != std::string::npos) {
                val = a.substr(eq + 1);
                a = a.substr(0, eq);
                has_eq = true;
            }
        } else if (a.size() > 2 && a[0] == '-' && a[1] != '-') { // -dVALUE
            val = a.substr(2);
            a = a.substr(0, 2);
            has_eq = true;
        }
        auto next = [&](std::string& out) -> bool {
            if (has_eq) {
                out = val;
                return true;
            }
            if (i + 1 < argc) {
                out = argv[++i];
                return true;
            }
            std::fprintf(stderr, "error: a value is required for '%s'\n", a.c_str());
            return false;
        };
        std::string v;
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "-V" || a == "--version") {
            std::printf("thunder_app %s\n%s\n", VERSION, OUR_VERSION);
            return 0;
        } else if (a == "--add-peer") {
            if (!next(v)) return 2;
            cfg.add_peers.push_back(v.rfind(':') == std::string::npos ? v + ":" + std::to_string(net::DEFAULT_PORT) : v);
        } else if (a == "-d" || a == "--datadir") {
            if (!next(v)) return 2;
            cfg.datadir = v;
        } else if (a == "--headless") {
            headless = true;
        } else if (a == "--log-dir") {
            if (!next(v)) return 2;
            log_dir = v;
        } else if (a == "--log-level" || a == "--log-level-file") {
            if (!next(v)) return 2;
            auto l = bt::log::parse_level(v);
            if (!l) {
                std::fprintf(stderr, "error: invalid log level '%s'\n", v.c_str());
                return 2;
            }
            (a == "--log-level" ? log_level : log_level_file) = *l;
        } else if (a == "--mainchain-grpc-url") {
            if (!next(v)) return 2;
            cfg.mainchain_grpc_url = v;
        } else if (a == "--mainchain-grpc-ca-cert") {
            if (!next(v)) return 2;
            cfg.mainchain_grpc_ca_cert = v;
        } else if (a == "--mnemonic-seed-phrase-path") {
            if (!next(v)) return 2;
            mnemonic_path = v;
        } else if (a == "-n" || a == "--net-addr") {
            if (!next(v) || !parse_sockaddr(v, cfg.net_addr)) {
                std::fprintf(stderr, "error: invalid --net-addr\n");
                return 2;
            }
        } else if (a == "--network") {
            if (!next(v)) return 2;
            if (v == "betanet") cfg.network = net::Network::Betanet;
            else if (v == "forknet") cfg.network = net::Network::Forknet;
            else if (v == "regtest") cfg.network = net::Network::Regtest;
            else if (v == "signet") cfg.network = net::Network::Signet;
            else {
                std::fprintf(stderr, "error: invalid value '%s' for '--network'\n", v.c_str());
                return 2;
            }
        } else if (a == "--network-magic") {
            if (!next(v)) return 2;
            auto m = bt::hex_decode(v);
            if (!m || m->size() != 4) {
                std::fprintf(stderr, "error: invalid --network-magic\n");
                return 2;
            }
            cfg.magic_override = std::array<uint8_t, 4>{(*m)[0], (*m)[1], (*m)[2], (*m)[3]};
        } else if (a == "-p" || a == "--private-rpc-addr") {
            if (!next(v) || !parse_sockaddr(v, private_rpc_addr)) return 2;
        } else if (a == "-r" || a == "--rpc-addr") {
            if (!next(v) || !parse_sockaddr(v, rpc_addr)) return 2;
        } else if (a == "--server-name") {
            if (!next(v)) return 2;
            cfg.server_names.push_back(v);
        } else if (a == "--wallet-dir") {
            if (!next(v)) return 2;
            wallet_dir = v;
        } else if (a == "--bt-no-seeds") {
            cfg.use_seeds = false;
        } else if (a == "--bt-no-p2p") {
            cfg.p2p = false;
        } else if (a == "--bt-wallet-derivation") {
            if (!next(v)) return 2;
            variant = v == "v0.18.0" ? bt::hd::Variant::V0_18_0 : bt::hd::Variant::V0_18_1;
        } else {
            unknown.push_back(argv[i]);
        }
    }
    if (!cfg.mainchain_grpc_ca_cert.empty() && cfg.mainchain_grpc_url.rfind("https://", 0) != 0) {
        std::fprintf(stderr, "Error: --mainchain-grpc-ca-cert needs an https --mainchain-grpc-url\n");
        return 2;
    }
    std::string logs = log_dir ? *log_dir : cfg.datadir + "/logs/v" + VERSION;
    bt::log::set_console_level(log_level);
    bt::log::set_file(logs, log_level_file);
    for (auto& u : unknown) LOG_WARN("ignoring unknown argument '%s'", u.c_str());
    if (!headless) LOG_INFO("no GUI in this build; running headless");
    LOG_INFO("%s starting: datadir %s, network magic override %s, mainchain %s", OUR_VERSION, cfg.datadir.c_str(),
             cfg.magic_override ? "yes" : "no", cfg.mainchain_grpc_url.c_str());

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    bt::frost::init();
    Node node(cfg);
    std::string err;
    if (!node.start(err)) {
        LOG_ERROR("startup failed: %s", err.c_str());
        return 1;
    }
    std::string wdir = wallet_dir ? *wallet_dir : cfg.datadir;
    std::filesystem::create_directories(wdir);
    Wallet wallet(wdir + "/backbone-wallet.sqlite", node, variant);
    if (mnemonic_path) {
        std::ifstream f(*mnemonic_path);
        std::string m((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        try {
            wallet.set_seed_from_mnemonic(m);
        } catch (const std::exception& e) {
            LOG_ERROR("mnemonic: %s", e.what());
            return 1;
        }
    }
    App app(node, wallet);
    app.on_stop = [] { g_stop = true; };
    bt::jsonrpc::Server pub, priv;
    if (private_rpc_addr == rpc_addr) {
        register_rpc(pub, app, true, true);
        if (!pub.listen(rpc_addr.ip, rpc_addr.port, err)) {
            LOG_ERROR("%s", err.c_str());
            return 1;
        }
    } else {
        register_rpc(pub, app, true, false);
        register_rpc(priv, app, false, true);
        if (!pub.listen(rpc_addr.ip, rpc_addr.port, err) || !priv.listen(private_rpc_addr.ip, private_rpc_addr.port, err)) {
            LOG_ERROR("%s", err.c_str());
            return 1;
        }
    }
    LOG_INFO("JSON-RPC on %s (%zu methods)", rpc_addr.to_string().c_str(), pub.methods().size());
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    LOG_INFO("shutting down");
    pub.stop();
    priv.stop();
    node.stop();
    return 0;
}
