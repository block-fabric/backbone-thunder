// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Small ordered key/value store with transactions, standing in for the LMDB tables the
// LayerTwo nodes use. A Txn sees its own writes, iterates in key byte order (as LMDB
// does) and commits atomically or is dropped.
#pragma once

#include <bt/bytes.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace bt::kv {

using Key = std::pair<uint8_t, Bytes>; // (table, key)

struct Batch {
    std::map<Key, std::optional<Bytes>> ops; // nullopt = delete
};

class Store {
public:
    virtual ~Store() = default;
    virtual std::optional<Bytes> get(uint8_t table, ByteSpan key) const = 0;
    /** Ordered scan of a table; stop when fn returns false. */
    virtual void scan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const = 0;
    /** Reverse-ordered scan. */
    virtual void rscan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const = 0;
    virtual void apply(const Batch& b) = 0;
};

class MemStore : public Store {
public:
    std::optional<Bytes> get(uint8_t table, ByteSpan key) const override;
    void scan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const override;
    void rscan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const override;
    void apply(const Batch& b) override;

private:
    std::map<Key, Bytes> m;
};

/** SQLite-backed store (WAL, one WITHOUT ROWID table). */
std::unique_ptr<Store> open_sqlite(const std::string& path);

class Txn {
public:
    explicit Txn(Store& s) : base(s) {}
    std::optional<Bytes> get(uint8_t table, ByteSpan key) const;
    void put(uint8_t table, ByteSpan key, ByteSpan val);
    /** Returns whether the key existed (heed's delete -> bool). */
    bool del(uint8_t table, ByteSpan key);
    void clear(uint8_t table);
    void scan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const;
    void rscan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const;
    std::optional<std::pair<Bytes, Bytes>> last(uint8_t table) const;
    void commit();
    void abort() { batch.ops.clear(); }
    Store& store() { return base; }

private:
    Store& base;
    Batch batch;
};

} // namespace bt::kv
