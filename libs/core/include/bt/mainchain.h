// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Mainchain view the sidechains consume, as the enforcer's cusf.mainchain.v1 reports it
// (validator.proto @ bip300301_enforcer 1753fc0c, the commit thunder-rust 0.18.2 pins).
#pragma once

#include <bt/bytes.h>

#include <optional>
#include <variant>
#include <vector>

class UniValue;

namespace bt::mainchain {

/** All hashes in internal (consensus) byte order. */
struct BlockHeaderInfo {
    Hash32 block_hash;
    Hash32 prev_block_hash;
    uint32_t height = 0;
    Hash32 work; // u256 little-endian total work
    uint64_t timestamp = 0;
};

struct Deposit {
    uint64_t sequence_number = 0; // their `tx_index`
    Hash32 txid;                  // mainchain outpoint, internal order
    uint32_t vout = 0;
    Bytes address;                // raw OP_RETURN bytes (the sidechain parses them)
    uint64_t value_sats = 0;
};

enum class BundleStatus : uint8_t { Submitted, Confirmed, Failed };

struct WithdrawalBundleEvent {
    Hash32 m6id; // bitcoin txid, internal order
    BundleStatus status = BundleStatus::Submitted;
    // Succeeded extras (ignored by thunder-rust's state machine, kept for RPC/indexing)
    std::optional<uint64_t> sequence_number;
    Bytes transaction;
};

using BlockEvent = std::variant<Deposit, WithdrawalBundleEvent>;

struct BlockInfo {
    std::optional<Hash32> bmm_commitment;
    std::vector<BlockEvent> events;
};

/** An ordered (oldest first) window of mainchain blocks with their per-slot info. */
struct TwoWayPegData {
    std::vector<std::pair<Hash32, BlockInfo>> block_info;
    /** LinkedHashMap::replace semantics (update in place or append). */
    void replace(const Hash32& h, const BlockInfo& bi);
};

// ---- Connect-JSON (protobuf canonical JSON) forms, as the public enforcer answers them.
std::optional<BlockHeaderInfo> header_info_from_json(const UniValue& v);
std::optional<BlockInfo> block_info_from_json(const UniValue& v);

} // namespace bt::mainchain
