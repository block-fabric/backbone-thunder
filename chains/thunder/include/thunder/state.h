// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Thunder state machine: a literal port of thunder-rust v0.18.2 lib/state/{mod,block,
// two_way_peg_data}.rs over our ordered KV store. Tables, key encodings and iteration
// order mirror their LMDB databases where the order is observable (e.g. deposit_blocks is
// keyed by little-endian u32 under memcmp order, exactly like their SerdeBincode<u32> key).
#pragma once

#include <array>
#include <map>
#include <thunder/types.h>

#include <bt/kv.h>
#include <bt/mainchain.h>

#include <list>
#include <stdexcept>
#include <unordered_map>

namespace thunder {

constexpr uint32_t WITHDRAWAL_BUNDLE_FAILURE_GAP = 4; // lib/state/mod.rs:37

/** A consensus failure. `panic` marks paths where their node panics instead of erroring;
 *  the network cannot connect such a block either, so we reject it too. */
struct StateError : std::runtime_error {
    bool panic = false;
    explicit StateError(const std::string& m, bool p = false) : std::runtime_error(m), panic(p) {}
};

enum class BundleStatus : uint8_t { Confirmed = 0, Dropped = 1, Failed = 2, Pending = 3, Submitted = 4, SubmittedUnexpected = 5 };
const char* to_string(BundleStatus s);

template <class T>
struct HeightStamped {
    T value;
    uint32_t height;
};
/** lib/state/rollback.rs: NonEmpty<HeightStamped<T>> */
template <class T>
struct RollBack {
    std::vector<HeightStamped<T>> v; // never empty
    static RollBack make(T x, uint32_t h) { return RollBack{{{x, h}}}; }
    const HeightStamped<T>& latest() const { return v.back(); }
    /** push fails if last().height > height */
    bool push(T x, uint32_t h)
    {
        if (v.back().height > h) return false;
        v.push_back({x, h});
        return true;
    }
    /** pop(): (Some(rest), last) or (None, head) */
    std::pair<std::optional<RollBack>, HeightStamped<T>> pop() const
    {
        if (v.size() > 1) {
            RollBack r = *this;
            auto last = r.v.back();
            r.v.pop_back();
            return {r, last};
        }
        return {std::nullopt, v.front()};
    }
};

/** types/lib.rs:90-292 */
struct WithdrawalBundle {
    std::map<OutPoint, Output> spend_utxos; // BTreeMap order
    std::vector<bt::btc::TxOut> outputs;     // full tx outputs: [fee, commitment, payouts...]
    Bytes tx_bytes() const { return bt::btc::serialize_inputless_tx(2, outputs, 0); }
    /** M6id = bitcoin txid (internal byte order). */
    Hash32 m6id() const { return bt::sha256d(tx_bytes()); }
};
/** WithdrawalBundle::new; nullopt if heavier than MAX_STANDARD_TX_WEIGHT. */
std::optional<WithdrawalBundle> make_withdrawal_bundle(uint32_t block_height, uint64_t fee, std::map<OutPoint, Output> spend_utxos,
                                                       std::vector<bt::btc::TxOut> payouts);
/** WithdrawalBundle::predict_weight (with one assumed treasury input). */
bool predict_weight_ok(uint32_t n_outputs, uint32_t sum_txout_sizes);

struct WithdrawalBundleInfo {
    enum Kind : uint8_t { Known = 0, Unknown = 1, UnknownConfirmed = 2 } kind = Unknown;
    WithdrawalBundle bundle;                // Known
    std::map<OutPoint, Output> spend_utxos; // UnknownConfirmed
};

/** Insertion-ordered set of +/- leaf changes (types/lib.rs:342-417, LinkedHashMap). */
class AccumulatorDiff {
public:
    void insert(const bt::utreexo::NodeHash& h);
    void remove(const bt::utreexo::NodeHash& h);
    void split(std::vector<bt::utreexo::NodeHash>& ins, std::vector<bt::utreexo::NodeHash>& dels) const;
    bool empty() const { return order.empty(); }

private:
    std::list<std::pair<bt::utreexo::NodeHash, bool>> order;
    std::unordered_map<bt::utreexo::NodeHash, std::list<std::pair<bt::utreexo::NodeHash, bool>>::iterator, bt::utreexo::NodeHashHasher> idx;
};

struct PrevalidatedBlock {
    std::vector<FilledTransaction> filled;
    Hash32 merkle_root;
    uint64_t total_fees = 0;
    uint64_t coinbase_value = 0;
    uint32_t next_height = 0;
    AccumulatorDiff diff;
    std::vector<bt::utreexo::NodeHash> roots_after; // accumulator roots after the block body
};

class State {
public:
    // table ids in the KV store
    enum Table : uint8_t {
        T_TIP = 1, T_HEIGHT, T_UTXOS, T_STXOS, T_PENDING_BUNDLE, T_LATEST_FAILED, T_BUNDLES,
        T_DEPOSIT_BLOCKS, T_BUNDLE_EVENT_BLOCKS, T_ACCUMULATOR
    };

    std::optional<Hash32> tip(const bt::kv::Txn& t) const;
    std::optional<uint32_t> height(const bt::kv::Txn& t) const;
    std::optional<Output> utxo(const bt::kv::Txn& t, const OutPoint& op) const;
    std::optional<SpentOutput> stxo(const bt::kv::Txn& t, const OutPoint& op) const;
    std::vector<std::pair<OutPoint, Output>> utxos(const bt::kv::Txn& t) const;
    std::vector<std::pair<OutPoint, SpentOutput>> stxos(const bt::kv::Txn& t) const;
    bt::utreexo::MemForest accumulator(const bt::kv::Txn& t) const;
    void put_accumulator(bt::kv::Txn& t, const bt::utreexo::MemForest& f) const;
    std::optional<Hash32> pending_bundle_m6id(const bt::kv::Txn& t) const;
    std::optional<std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>> bundle(const bt::kv::Txn& t, const Hash32& m6id) const;
    std::vector<std::pair<Hash32, std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>>> bundles(const bt::kv::Txn& t) const;
    /** (pending bundle, height of its latest status) */
    std::optional<std::pair<WithdrawalBundle, uint32_t>> pending_bundle(const bt::kv::Txn& t) const;
    /** (failure height, m6id) */
    std::optional<std::pair<uint32_t, Hash32>> latest_failed_bundle(const bt::kv::Txn& t) const;
    std::optional<RollBack<Hash32>> latest_failed_rollback(const bt::kv::Txn& t) const;
    /** deposit_blocks / withdrawal_bundle_event_blocks in memcmp(LE key) order */
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> deposit_blocks(const bt::kv::Txn& t) const;
    std::vector<std::pair<uint32_t, std::pair<Hash32, uint32_t>>> bundle_event_blocks(const bt::kv::Txn& t) const;
    uint64_t sidechain_wealth(const bt::kv::Txn& t) const;

    // ---- validation / connection (lib/state/block.rs)
    /** Options exist only for replaying public index data, which omits the (uncommitted)
     *  signatures and the header roots; every other caller uses the defaults. */
    struct CheckOpts {
        bool authorizations = true;
        bool roots = true;
    };
    PrevalidatedBlock prevalidate(const bt::kv::Txn& t, const Header& h, const Body& b, CheckOpts opts) const;
    PrevalidatedBlock prevalidate(const bt::kv::Txn& t, const Header& h, const Body& b) const { return prevalidate(t, h, b, CheckOpts{}); }
    void connect_prevalidated(bt::kv::Txn& t, const Header& h, const Body& b, const PrevalidatedBlock& pre) const;
    void disconnect_tip(bt::kv::Txn& t, const Header& h, const Body& b) const;
    /** State::validate_transaction (mempool / submit_transaction); returns the fee. */
    uint64_t validate_transaction(const bt::kv::Txn& t, const AuthorizedTransaction& tx) const;
    /** validate_filled_transaction: fee of a filled tx, or throws. */
    static uint64_t validate_filled(const FilledTransaction& ft);

    // ---- two-way peg (lib/state/two_way_peg_data.rs)
    void connect_2wpd(bt::kv::Txn& t, const bt::mainchain::TwoWayPegData& d) const;
    void disconnect_2wpd(bt::kv::Txn& t, const bt::mainchain::TwoWayPegData& d) const;
    std::optional<WithdrawalBundle> collect_withdrawal_bundle(const bt::kv::Txn& t, uint32_t block_height) const;

    /** proto.rs:446-506: deposit address = OP_RETURN bytes as UTF-8 base58 of 20 bytes, else ALL_ZEROS. */
    static Output deposit_output(const bt::mainchain::Deposit& d);

private:
    void put_bundle(bt::kv::Txn& t, const Hash32& m6id, const WithdrawalBundleInfo& info, const RollBack<BundleStatus>& st) const;
    void connect_bundle_submitted(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& event_block, const Hash32& m6id) const;
    void connect_bundle_confirmed(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& event_block, const Hash32& m6id) const;
    void connect_bundle_failed(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& m6id) const;
    void disconnect_bundle_submitted(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& m6id) const;
    void disconnect_bundle_confirmed(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& m6id) const;
    void disconnect_bundle_failed(bt::kv::Txn& t, uint32_t h, AccumulatorDiff& diff, const Hash32& m6id) const;
    void apply_diff(bt::kv::Txn& t, const AccumulatorDiff& d) const;
};

// value encodings (also used by the archive)
void enc_bundle_info(Writer& w, const WithdrawalBundleInfo& i);
WithdrawalBundleInfo dec_bundle_info(Reader& r);

/**
 * lib/mempool.rs MemPool::put: an input the mempool already spends is refused (UtxoDoubleSpent); theirs records each
 * input as it checks it, so an outpoint a transaction spends twice is refused at its second time, as in a block
 * (block.rs, the sorted input keys). Nothing is changed when refused.
 */
void mempool_check_inputs(const std::map<std::array<uint8_t, 37>, Hash32>& spent, const Transaction& tx);

} // namespace thunder
