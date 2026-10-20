// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Thunder wallet: seed from a BIP-39 mnemonic, FROST/ristretto255 keys at
// m/43'/1899'/0'/9'/0'/i (thunder-rust lib/wallet), addresses from index 1, coin
// selection and transaction building as theirs. UTXOs are read from node state by
// address (their wallet keeps a mirror; the result is the same set).
#pragma once

#include <thunder/node.h>

#include <bt/hd.h>
#include <bt/kv.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace thunder {

struct Balance {
    uint64_t total = 0, available = 0;
};

class Wallet {
public:
    Wallet(const std::string& path, Node& node, bt::hd::Variant variant = bt::hd::Variant::V0_18_1);
    bool has_seed();
    /** set_seed: error if a different seed exists. */
    void set_seed(const bt::ByteArray<64>& seed);
    void set_seed_from_mnemonic(const std::string& mnemonic);
    Address get_new_address();
    Address get_or_generate_last_address();
    std::vector<Address> addresses();
    std::vector<std::pair<OutPoint, Output>> utxos();
    Balance balance();
    Transaction create_transaction_many(const std::map<Address, uint64_t>& dests, uint64_t fee);
    Transaction create_withdrawal(const bt::btc::Address& main_address, uint64_t value, uint64_t main_fee, uint64_t fee);
    AuthorizedTransaction authorize(const Transaction& tx);
    bt::hd::Variant variant() const { return variant_; }

private:
    std::unique_ptr<bt::kv::Store> store_;
    Node& node_;
    bt::hd::Variant variant_;
    std::mutex mu_;
    std::optional<bt::ByteArray<64>> seed();
    bt::frost::SigningKey key(uint32_t index);
    std::pair<uint64_t, std::vector<std::pair<OutPoint, Output>>> select_coins(uint64_t value);
};

} // namespace thunder
