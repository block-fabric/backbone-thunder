// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Rebuilds Thunder blocks from public betanet data (the seed index esplora facade at
// seed.beta.ecash.eu.com/thunder plus the public enforcer's GetTwoWayPegData for slot 9)
// and replays them through the state machine. The index publishes headers without their
// Utreexo roots and bodies without signatures or coinbase; we recompute the roots, infer
// the coinbase where the data allows it, and compare every recomputed header hash with
// the block id the index reports.
#pragma once

#include <thunder/state.h>

#include <bt/mainchain.h>

#include <univalue.h>

#include <map>
#include <string>
#include <vector>

namespace thunder::fixture {

struct IndexBlock {
    uint32_t height = 0;
    Hash32 id;
    Hash32 merkle_root;
    std::optional<Hash32> prev;
    Hash32 prev_main; // internal order
    uint32_t main_height = 0;
    uint64_t fees = 0;
    std::vector<UniValue>* txs = nullptr;
};

struct MainBlock {
    bt::mainchain::BlockHeaderInfo header;
    bt::mainchain::BlockInfo info;
};

struct Data {
    std::vector<IndexBlock> blocks;            // by height
    std::map<uint32_t, std::vector<UniValue>> txs; // height -> esplora tx json
    std::map<std::string, std::string> txhex;  // txid -> borsh hex
    std::map<uint32_t, MainBlock> main_by_height; // enforcer 2WPD rows (blocks with BMM or events)
    std::map<Hash32, uint32_t> main_height_of;
};

/** Load blocks.jsonl, txs.jsonl, txhex.jsonl and twpd9.json from dir. */
std::optional<Data> load(const std::string& dir, std::string& err);

struct ReplayResult {
    uint32_t verified_hashes = 0;       // header hashes recomputed and equal to the index id
    uint32_t verified_merkle_roots = 0; // bodies whose merkle root matched
    uint32_t bmm_verified = 0;          // blocks whose BMM commitment is in the child of prev_main
    uint32_t first_unknown_coinbase = UINT32_MAX;
    uint32_t replayed_to = 0;
    std::string error;
    std::vector<std::string> notes;
};

/** Replay from genesis; stops at the first block it cannot rebuild exactly. */
ReplayResult replay(const Data& d, State& st, bt::kv::Store& store, uint32_t max_height = UINT32_MAX);

/** Full blocks as fetched from public peers (thunder-fetch-blocks: u32 height, u32 len,
 *  bincode(ResponseMessage::Block)). */
std::vector<std::pair<uint32_t, Bytes>> load_blocks_bin(const std::string& path);

struct FullReplayResult {
    uint32_t blocks = 0;            // connected with every consensus check enabled
    uint32_t reencoded_exact = 0;   // bincode re-encoding identical to the bytes received
    uint32_t hashes_match_index = 0;
    uint32_t signatures = 0;        // FROST signatures verified
    uint32_t proofs = 0;            // non-empty Utreexo proofs verified
    uint32_t bmm_verified = 0;
    uint32_t deposits = 0;
    std::string error;
};
struct ReplayStep {
    Block block;
    bt::mainchain::TwoWayPegData window;
    Bytes accumulator; // serialized forest after the block and its 2WPD
};
/** Replay real blocks with all checks (signatures, Utreexo proofs and roots, merkle, BMM). */
FullReplayResult replay_full(const Data& d, const std::vector<std::pair<uint32_t, Bytes>>& blocks, State& st, bt::kv::Store& store,
                             std::vector<ReplayStep>* steps = nullptr);

} // namespace thunder::fixture
