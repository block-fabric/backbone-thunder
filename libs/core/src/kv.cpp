// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/kv.h>

#include <sqlite3.h>
#include <stdexcept>
#include <vector>

namespace bt::kv {

std::optional<Bytes> MemStore::get(uint8_t table, ByteSpan key) const
{
    auto it = m.find(Key{table, Bytes(key.begin(), key.end())});
    if (it == m.end()) return std::nullopt;
    return it->second;
}

void MemStore::scan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const
{
    for (auto it = m.lower_bound(Key{table, {}}); it != m.end() && it->first.first == table; ++it)
        if (!fn(it->first.second, it->second)) return;
}

void MemStore::rscan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const
{
    std::vector<std::map<Key, Bytes>::const_iterator> its;
    for (auto it = m.lower_bound(Key{table, {}}); it != m.end() && it->first.first == table; ++it) its.push_back(it);
    for (auto r = its.rbegin(); r != its.rend(); ++r)
        if (!fn((*r)->first.second, (*r)->second)) return;
}

void MemStore::apply(const Batch& b)
{
    for (auto& [k, v] : b.ops) {
        if (v) m[k] = *v;
        else m.erase(k);
    }
}

// ---------------------------------------------------------------- Txn
std::optional<Bytes> Txn::get(uint8_t table, ByteSpan key) const
{
    auto it = batch.ops.find(Key{table, Bytes(key.begin(), key.end())});
    if (it != batch.ops.end()) return it->second;
    return base.get(table, key);
}

void Txn::put(uint8_t table, ByteSpan key, ByteSpan val) { batch.ops[Key{table, Bytes(key.begin(), key.end())}] = Bytes(val.begin(), val.end()); }

bool Txn::del(uint8_t table, ByteSpan key)
{
    bool existed = get(table, key).has_value();
    batch.ops[Key{table, Bytes(key.begin(), key.end())}] = std::nullopt;
    return existed;
}

void Txn::clear(uint8_t table)
{
    std::vector<Bytes> keys;
    scan(table, [&](ByteSpan k, ByteSpan) {
        keys.emplace_back(k.begin(), k.end());
        return true;
    });
    for (auto& k : keys) batch.ops[Key{table, k}] = std::nullopt;
}

// Merge the base store's ordered entries with this txn's overlay.
void Txn::scan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const
{
    std::map<Bytes, Bytes> merged;
    base.scan(table, [&](ByteSpan k, ByteSpan v) {
        merged.emplace(Bytes(k.begin(), k.end()), Bytes(v.begin(), v.end()));
        return true;
    });
    for (auto it = batch.ops.lower_bound(Key{table, {}}); it != batch.ops.end() && it->first.first == table; ++it) {
        if (it->second) merged[it->first.second] = *it->second;
        else merged.erase(it->first.second);
    }
    for (auto& [k, v] : merged)
        if (!fn(k, v)) return;
}

void Txn::rscan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const
{
    std::map<Bytes, Bytes> merged;
    scan(table, [&](ByteSpan k, ByteSpan v) {
        merged.emplace(Bytes(k.begin(), k.end()), Bytes(v.begin(), v.end()));
        return true;
    });
    for (auto it = merged.rbegin(); it != merged.rend(); ++it)
        if (!fn(it->first, it->second)) return;
}

std::optional<std::pair<Bytes, Bytes>> Txn::last(uint8_t table) const
{
    std::optional<std::pair<Bytes, Bytes>> r;
    rscan(table, [&](ByteSpan k, ByteSpan v) {
        r = std::make_pair(Bytes(k.begin(), k.end()), Bytes(v.begin(), v.end()));
        return false;
    });
    return r;
}

void Txn::commit()
{
    base.apply(batch);
    batch.ops.clear();
}

// ---------------------------------------------------------------- SQLite
namespace {
class SqliteStore : public Store {
public:
    explicit SqliteStore(const std::string& path)
    {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) throw std::runtime_error("sqlite open " + path);
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=FULL");
        exec("CREATE TABLE IF NOT EXISTS kv (t INTEGER NOT NULL, k BLOB NOT NULL, v BLOB NOT NULL, PRIMARY KEY (t, k)) WITHOUT ROWID");
        sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE t=? AND k=?", -1, &q_get, nullptr);
        sqlite3_prepare_v2(db, "SELECT k, v FROM kv WHERE t=? ORDER BY k ASC", -1, &q_scan, nullptr);
        sqlite3_prepare_v2(db, "SELECT k, v FROM kv WHERE t=? ORDER BY k DESC", -1, &q_rscan, nullptr);
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO kv (t, k, v) VALUES (?, ?, ?)", -1, &q_put, nullptr);
        sqlite3_prepare_v2(db, "DELETE FROM kv WHERE t=? AND k=?", -1, &q_del, nullptr);
    }
    ~SqliteStore() override
    {
        for (auto s : {q_get, q_scan, q_rscan, q_put, q_del}) sqlite3_finalize(s);
        sqlite3_close(db);
    }
    std::optional<Bytes> get(uint8_t table, ByteSpan key) const override
    {
        sqlite3_reset(q_get);
        sqlite3_bind_int(q_get, 1, table);
        sqlite3_bind_blob(q_get, 2, key.data(), int(key.size()), SQLITE_STATIC);
        std::optional<Bytes> r;
        if (sqlite3_step(q_get) == SQLITE_ROW) {
            auto p = (const uint8_t*)sqlite3_column_blob(q_get, 0);
            int n = sqlite3_column_bytes(q_get, 0);
            r = Bytes(p, p + n);
        }
        sqlite3_reset(q_get);
        return r;
    }
    void scan_impl(sqlite3_stmt* q, uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const
    {
        // Materialise first so that callbacks may re-enter the store.
        std::vector<std::pair<Bytes, Bytes>> rows;
        sqlite3_reset(q);
        sqlite3_bind_int(q, 1, table);
        while (sqlite3_step(q) == SQLITE_ROW) {
            auto kp = (const uint8_t*)sqlite3_column_blob(q, 0);
            int kn = sqlite3_column_bytes(q, 0);
            auto vp = (const uint8_t*)sqlite3_column_blob(q, 1);
            int vn = sqlite3_column_bytes(q, 1);
            rows.emplace_back(Bytes(kp, kp + kn), Bytes(vp, vp + vn));
        }
        sqlite3_reset(q);
        for (auto& [k, v] : rows)
            if (!fn(k, v)) return;
    }
    void scan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const override { scan_impl(q_scan, table, fn); }
    void rscan(uint8_t table, const std::function<bool(ByteSpan, ByteSpan)>& fn) const override { scan_impl(q_rscan, table, fn); }
    void apply(const Batch& b) override
    {
        exec("BEGIN IMMEDIATE");
        for (auto& [k, v] : b.ops) {
            sqlite3_stmt* q = v ? q_put : q_del;
            sqlite3_reset(q);
            sqlite3_bind_int(q, 1, k.first);
            sqlite3_bind_blob(q, 2, k.second.data(), int(k.second.size()), SQLITE_STATIC);
            if (v) sqlite3_bind_blob(q, 3, v->data(), int(v->size()), SQLITE_STATIC);
            if (sqlite3_step(q) != SQLITE_DONE) {
                exec("ROLLBACK");
                throw std::runtime_error(std::string("sqlite write: ") + sqlite3_errmsg(db));
            }
        }
        exec("COMMIT");
    }

private:
    void exec(const char* sql)
    {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string e = err ? err : "?";
            sqlite3_free(err);
            throw std::runtime_error("sqlite: " + e);
        }
    }
    sqlite3* db = nullptr;
    sqlite3_stmt *q_get = nullptr, *q_scan = nullptr, *q_rscan = nullptr, *q_put = nullptr, *q_del = nullptr;
};
} // namespace

std::unique_ptr<Store> open_sqlite(const std::string& path) { return std::make_unique<SqliteStore>(path); }

} // namespace bt::kv
