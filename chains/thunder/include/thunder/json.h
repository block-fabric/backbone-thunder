// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// JSON forms of Thunder types exactly as serde_json renders them in thunder-rust's
// JSON-RPC (human-readable serde): hex hashes, base58 addresses, externally tagged
// enums, [u8; 32] as arrays of numbers where serde does that, etc.
#pragma once

#include <thunder/state.h>

#include <univalue.h>

namespace thunder::json {

UniValue hash(const Hash32& h);            // hex (BlockHash, Txid, MerkleRoot, ...)
UniValue btc_hash(const Hash32& h);        // display order (bitcoin::BlockHash, Txid, M6id)
UniValue bytes_array(ByteSpan b);          // serde of [u8; N] / Vec<u8>: array of numbers
UniValue address(const Address& a);
UniValue outpoint(const OutPoint& o);
UniValue content(const Content& c);
UniValue output(const Output& o);
UniValue pointed_output(const OutPoint& op, const Output& o);
UniValue spent_output(const SpentOutput& s);
UniValue pointed_spent_output(const OutPoint& op, const SpentOutput& s);
UniValue node_hash(const bt::utreexo::NodeHash& n);
UniValue proof(const bt::utreexo::Proof& p);
UniValue transaction(const Transaction& t);
UniValue authorization(const Authorization& a);
UniValue authorized_transaction(const AuthorizedTransaction& t);
UniValue coinbase(const Coinbase& c);
UniValue body(const Body& b, bool verbose);
UniValue header(const Header& h);
UniValue block(const Header& h, const Body& b, bool verbose = false);
UniValue bitcoin_tx(const std::vector<bt::btc::TxOut>& outs);
UniValue withdrawal_bundle(const WithdrawalBundle& w);
UniValue withdrawal_bundle_info(const WithdrawalBundleInfo& i);

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
Hash32 parse_hash(const UniValue& v);
Hash32 parse_btc_hash(const UniValue& v);
Address parse_address(const UniValue& v);
bt::btc::Address parse_btc_address(const UniValue& v);
OutPoint parse_outpoint(const UniValue& v);
Output parse_output(const UniValue& v);
bt::utreexo::Proof parse_proof(const UniValue& v);
Transaction parse_transaction(const UniValue& v);
AuthorizedTransaction parse_authorized_transaction(const UniValue& v);
Header parse_header(const UniValue& v);
Body parse_body(const UniValue& v);
uint64_t parse_u64(const UniValue& v);

} // namespace thunder::json
