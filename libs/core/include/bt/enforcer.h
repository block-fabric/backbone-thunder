// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Typed client for cusf.mainchain.v1 (bip300301_enforcer @1753fc0c, the version
// thunder-rust 0.18.2 pins), with the same requests their node makes
// (thunder-rust lib/types/proto.rs:1001-1367, lib/node/mainchain_task.rs).
// Works against a real enforcer or any server implementing the same services
// (backboned). gRPC over HTTP/2 (h2c or TLS).
#pragma once

#include <bt/grpc.h>
#include <bt/mainchain.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bt::enforcer {

struct Error {
    grpc::Status status;
    std::string what() const { return "enforcer: " + std::to_string(status.code) + " " + status.message; }
};

template <class T>
using Result = std::variant<T, Error>;
template <class T>
bool ok(const Result<T>& r) { return r.index() == 0; }
template <class T>
const T& val(const Result<T>& r) { return std::get<0>(r); }
template <class T>
const Error& err(const Result<T>& r) { return std::get<1>(r); }

struct ChainInfo {
    int network = 0; // NETWORK_* enum value
    std::optional<uint32_t> activation_height;
};

struct SubscribeEvent {
    enum Kind { Connect, Disconnect } kind = Connect;
    mainchain::BlockHeaderInfo header;
    mainchain::BlockInfo info;
    Hash32 disconnected; // Disconnect
};

class Client {
public:
    Client(const std::string& url, uint8_t sidechain, std::string ca_file = "");

    /** grpc.health.v1.Health/Check for a service; nullopt if the server lacks Health. */
    std::optional<bool> health(const std::string& service);
    Result<mainchain::BlockHeaderInfo> get_chain_tip();
    Result<ChainInfo> get_chain_info();
    /** GetBlockHeaderInfo(block, max_ancestors); empty when the block is unknown.
     *  Checks the ancestors are a contiguous prev-hash chain (proto.rs:1103-1140). */
    Result<std::vector<mainchain::BlockHeaderInfo>> get_block_header_infos(const Hash32& block, uint32_t max_ancestors);
    /** GetBlockInfo(block, sidechain, max_ancestors), newest first; contiguity checked. */
    Result<std::vector<std::pair<mainchain::BlockHeaderInfo, mainchain::BlockInfo>>> get_block_infos(const Hash32& block, uint32_t max_ancestors);
    /** GetBmmHStarCommitment: newest first; nullopt entries = no commitment; error if block unknown. */
    Result<std::vector<std::optional<Hash32>>> get_bmm_hstar_commitments(const Hash32& block, uint32_t max_ancestors);
    Result<std::vector<std::pair<mainchain::BlockHeaderInfo, mainchain::BlockInfo>>> get_two_way_peg_data(const std::optional<Hash32>& start,
                                                                                                         const Hash32& end);
    /** SubscribeEvents for our slot; blocks until the stream ends, on_event false cancels. */
    grpc::Status subscribe_events(const std::function<bool(const SubscribeEvent&)>& on_event, const std::atomic<bool>* cancel = nullptr);

    // wallet / mining / block producer (used by `mine`, `create_deposit`, bundles)
    Result<Hash32> create_bmm_critical_data_tx(uint64_t value_sats, uint32_t height, const Hash32& critical_hash, const Hash32& prev_bytes);
    Result<Hash32> create_deposit_tx(const std::string& address, uint64_t value_sats, uint64_t fee_sats);
    Result<std::string> create_new_address();
    Result<std::vector<Hash32>> generate_to_address(uint32_t blocks, const std::string& address);
    Result<bool> propose_withdrawal_bundle(const Bytes& tx);

    const std::string& url() const { return url_; }

private:
    std::string url_, ca_;
    uint8_t slot_;
    std::unique_ptr<grpc::Channel> ch_;
};

/** Parse protobuf BlockHeaderInfo / BlockInfo (shared with servers and tests). */
std::optional<mainchain::BlockHeaderInfo> parse_header_info(ByteSpan pb);
std::optional<mainchain::BlockInfo> parse_block_info(ByteSpan pb);

} // namespace bt::enforcer
