// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Literal port of rustreexo rev 6da503b (MIT): src/accumulator/{util,proof,mem_forest,node_hash}.rs.
// Rust release-mode integer semantics: wrapping arithmetic, shift amount masked to the type width.
#include <bt/codec.h>
#include <bt/crypto.h>
#include <bt/utreexo.h>

#include <algorithm>

namespace bt::utreexo {

namespace {
inline uint64_t shl(uint64_t x, unsigned n) { return x << (n & 63); }
inline uint64_t shr(uint64_t x, unsigned n) { return x >> (n & 63); }
inline uint8_t u8w(int v) { return uint8_t(v); } // u8 wrapping
} // namespace

static const Hash32 ZERO32{};

const Hash32& NodeHash::bytes() const { return tag == Some ? h : ZERO32; }

std::string NodeHash::to_string() const { return tag == Some ? hex_encode(h) : "empty"; }

NodeHash parent_hash(const NodeHash& l, const NodeHash& r)
{
    Bytes b;
    b.reserve(64);
    append(b, l.bytes().span());
    append(b, r.bytes().span());
    return NodeHash::some(sha512_256(b));
}

// ------------------------------------------------------------------ util.rs
namespace util {

uint8_t tree_rows(uint64_t n)
{
    if (n == 0) return 0;
    uint64_t m = n - 1;
    int lz = m == 0 ? 64 : __builtin_clzll(m);
    return uint8_t(64 - lz);
}

uint8_t detect_row(uint64_t pos, uint8_t forest_rows)
{
    uint64_t marker = shl(1, forest_rows);
    uint8_t h = 0;
    while (pos & marker) {
        marker >>= 1;
        h = u8w(h + 1);
    }
    return h;
}

uint64_t root_position(uint64_t num_leaves, uint8_t row, uint8_t forest_rows)
{
    uint64_t mask = shl(2, forest_rows) - 1;
    uint64_t before = num_leaves & shl(mask, u8w(row + 1));
    uint64_t shifted = shr(before, row) | shl(mask, u8w(forest_rows + 1 - row));
    return shifted & mask;
}

bool is_root_position(uint64_t position, uint64_t num_leaves, uint8_t forest_rows)
{
    uint8_t row = detect_row(position, forest_rows);
    bool root_present = (num_leaves & shl(1, row)) != 0;
    uint64_t root_pos = root_position(num_leaves, row, forest_rows);
    return root_present && root_pos == position;
}

uint64_t parent(uint64_t pos, uint8_t forest_rows) { return (pos >> 1) | shl(1, forest_rows); }

static uint64_t children(uint64_t pos, uint8_t forest_rows)
{
    uint64_t mask = shl(2, forest_rows) - 1;
    return (pos << 1) & mask;
}
uint64_t left_child(uint64_t pos, uint8_t forest_rows) { return children(pos, forest_rows); }
uint64_t right_child(uint64_t pos, uint8_t forest_rows) { return children(pos, forest_rows) + 1; }

static bool is_root_populated(uint8_t row, uint64_t num_leaves) { return (shr(num_leaves, row) & 1) == 1; }

static bool is_sibling(uint64_t a, uint64_t b) { return (a ^ 1) == b; }

std::vector<uint64_t> get_proof_positions(const std::vector<uint64_t>& targets, uint64_t num_leaves, uint8_t forest_rows)
{
    std::vector<uint64_t> proof_positions;
    std::vector<uint64_t> computed = targets;
    std::sort(computed.begin(), computed.end());
    for (unsigned row = 0; row <= forest_rows; row++) {
        std::vector<uint64_t> row_targets;
        for (uint64_t x : computed)
            if (detect_row(x, forest_rows) == row) row_targets.push_back(x);
        size_t i = 0;
        while (i < row_targets.size()) {
            uint64_t node = row_targets[i++];
            if (is_root_position(node, num_leaves, forest_rows)) continue;
            if (i < row_targets.size()) {
                uint64_t next = row_targets[i];
                if (!is_sibling(node, next)) proof_positions.push_back(node ^ 1);
                else i++;
            } else {
                proof_positions.push_back(node ^ 1);
            }
            computed.push_back(parent(node, forest_rows));
        }
        std::sort(computed.begin(), computed.end());
    }
    return proof_positions;
}

Offset detect_offset(uint64_t pos, uint64_t num_leaves)
{
    uint8_t tr = tree_rows(num_leaves);
    uint8_t nr = detect_row(pos, tr);
    uint8_t bigger_trees = 0;
    uint64_t marker = pos;
    while ((shl(marker, nr) & (shl(2, tr) - 1)) >= (shl(1, tr) & num_leaves)) {
        uint64_t tree_size = shl(1, tr) & num_leaves;
        if (tree_size != 0) {
            marker -= tree_size;
            bigger_trees = u8w(bigger_trees + 1);
        }
        tr = u8w(tr - 1);
    }
    return Offset{bigger_trees, u8w(tr - nr), ~marker};
}

} // namespace util

// ------------------------------------------------------------------ proof.rs
std::optional<bool> Proof::verify(const std::vector<NodeHash>& del_hashes, const std::vector<NodeHash>& roots,
                                  uint64_t num_leaves) const
{
    if (targets.empty()) return true;
    // calculate_hashes
    uint8_t total_rows = util::tree_rows(num_leaves);
    std::vector<NodeHash> calculated_roots;
    std::vector<uint64_t> proof_positions = util::get_proof_positions(targets, num_leaves, total_rows);
    std::vector<std::pair<uint64_t, NodeHash>> nodes;
    for (size_t i = 0; i < targets.size() && i < del_hashes.size(); i++) nodes.emplace_back(targets[i], del_hashes[i]);
    for (size_t i = 0; i < proof_positions.size() && i < hashes.size(); i++) nodes.emplace_back(proof_positions[i], hashes[i]);
    std::sort(nodes.begin(), nodes.end());
    std::vector<std::pair<uint64_t, NodeHash>> computed;
    size_t ci = 0, pi = 0;
    auto get_next = [&]() -> std::optional<std::pair<uint64_t, NodeHash>> {
        const std::pair<uint64_t, NodeHash>* c = ci < computed.size() ? &computed[ci] : nullptr;
        const std::pair<uint64_t, NodeHash>* p = pi < nodes.size() ? &nodes[pi] : nullptr;
        if (c && p) {
            if (c->first < p->first) return computed[ci++];
            return nodes[pi++];
        }
        if (c) return computed[ci++];
        if (p) return nodes[pi++];
        return std::nullopt;
    };
    while (auto next = get_next()) {
        auto [next_pos, next_hash] = *next;
        if (util::is_root_position(next_pos, num_leaves, total_rows)) {
            calculated_roots.push_back(next_hash);
            continue;
        }
        uint64_t sibling = next_pos | 1;
        auto sib = get_next();
        if (!sib) return std::nullopt; // Missing sibling
        if (sib->first != sibling) return std::nullopt;
        NodeHash ph = parent_hash(next_hash, sib->second);
        computed.emplace_back(util::parent(next_pos, total_rows), ph);
    }
    // root matching
    size_t idx = 0, matched = 0;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        if (idx < calculated_roots.size() && calculated_roots[idx] == *it) {
            matched++;
            idx++;
        }
    }
    size_t remaining = calculated_roots.size() - idx;
    if (remaining != matched && remaining != 0) return false;
    return true;
}

// ------------------------------------------------------------------ mem_forest.rs
void MemForest::Node::recompute_hashes()
{
    if (left && right) data = parent_hash(left->data, right->data);
    if (has_parent) {
        if (auto p = parent.lock()) p->recompute_hashes();
    }
}

static void write_one(const MemForest::Node& n, Writer& w)
{
    w.u64(n.leaf ? 1 : 0);
    w.u8(uint8_t(n.data.tag));
    if (n.data.tag == NodeHash::Some) w.raw(n.data.h);
    if (n.left) write_one(*n.left, w);
    if (n.right) write_one(*n.right, w);
}

Bytes MemForest::serialize() const
{
    Writer w;
    w.u64(leaves);
    w.u64(roots.size());
    for (auto& r : roots) write_one(*r, w);
    return w.buf;
}

static NodeHash read_hash(Reader& r)
{
    uint8_t tag = r.u8();
    if (tag == 0) return NodeHash::empty();
    if (tag == 1) return NodeHash{NodeHash::Placeholder, {}};
    if (tag == 2) return NodeHash::some(r.array<32>());
    throw DecodeError("unexpected tag for AccumulatorHash");
}

using NodePtr = MemForest::NodePtr;

static NodePtr read_one(Reader& r, const NodePtr& ancestor, std::vector<std::pair<NodeHash, std::weak_ptr<MemForest::Node>>>& index, int depth)
{
    if (depth > 128) throw DecodeError("forest too deep");
    uint64_t ty = r.u64();
    NodeHash data = read_hash(r);
    if (ty > 1) throw DecodeError("Invalid node type"); // theirs panics
    auto node = std::make_shared<MemForest::Node>();
    node->leaf = ty == 1;
    node->data = data;
    if (ancestor) {
        node->parent = ancestor;
        node->has_parent = true;
    }
    if (node->leaf) {
        index.emplace_back(data, node);
        return node;
    }
    if (!data.is_empty()) {
        node->left = read_one(r, node, index, depth + 1);
        node->right = read_one(r, node, index, depth + 1);
    }
    return node;
}

std::optional<MemForest> MemForest::deserialize(ByteSpan b)
{
    try {
        Reader r(b);
        MemForest f;
        f.leaves = r.u64();
        uint64_t n = r.u64();
        if (n > 64) return std::nullopt;
        std::vector<std::pair<NodeHash, std::weak_ptr<Node>>> index;
        for (uint64_t i = 0; i < n; i++) f.roots.push_back(read_one(r, nullptr, index, 0));
        for (auto& [h, w] : index) f.map[h] = w; // HashMap::extend: last wins
        return f;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

MemForest& MemForest::operator=(const MemForest& o)
{
    if (this == &o) return *this;
    auto f = deserialize(o.serialize());
    *this = std::move(*f);
    return *this;
}

void MemForest::rebuild_map()
{
    map.clear();
    std::function<void(const NodePtr&)> walk = [&](const NodePtr& n) {
        if (n->leaf) {
            map[n->data] = n;
            return;
        }
        if (n->left) walk(n->left);
        if (n->right) walk(n->right);
    };
    for (auto& r : roots) walk(r);
}

std::vector<NodeHash> MemForest::get_roots() const
{
    std::vector<NodeHash> r;
    for (auto& n : roots) r.push_back(n->data);
    return r;
}

std::optional<uint64_t> MemForest::get_pos(const std::weak_ptr<Node>& wnode) const
{
    uint64_t left_child_indicator = 0;
    uint64_t rows_to_top = 0;
    NodePtr node = wnode.lock();
    if (!node) return std::nullopt;
    while (node->has_parent) {
        NodePtr parent = node->parent.lock();
        if (!parent || !parent->left) return std::nullopt; // "Could not upgrade parent"
        if (parent->left->data == node->data) {
            left_child_indicator <<= 1;
        } else {
            left_child_indicator <<= 1;
            left_child_indicator |= 1;
        }
        rows_to_top++;
        node = parent;
    }
    if (roots.empty()) return std::nullopt;
    size_t root_idx = roots.size() - 1;
    uint8_t forest_rows = util::tree_rows(leaves);
    std::optional<uint8_t> root_row;
    for (unsigned row = 0; row <= forest_rows; row++) {
        if (util::is_root_populated(uint8_t(row), leaves)) {
            if (root_idx >= roots.size()) return std::nullopt; // index panic in Rust
            const auto& root = roots[root_idx];
            if (root->data == node->data) {
                root_row = uint8_t(row);
                break;
            }
            root_idx -= 1; // wraps like usize in release; next access fails
        }
    }
    if (!root_row) return std::nullopt;
    uint64_t pos = util::root_position(leaves, *root_row, forest_rows);
    for (uint64_t i = 0; i < rows_to_top; i++) {
        if ((left_child_indicator & 1) == 0) pos = util::left_child(pos, forest_rows);
        else pos = util::right_child(pos, forest_rows);
        left_child_indicator >>= 1;
    }
    return pos;
}

std::optional<MemForest::Grab> MemForest::grab_node(uint64_t pos) const
{
    auto off = util::detect_offset(pos, leaves);
    if (off.tree >= roots.size()) return std::nullopt; // index panic in Rust
    NodePtr n = roots[off.tree];
    NodePtr sibling = roots[off.tree];
    NodePtr parent = sibling;
    for (int row = int(off.branch_len) - 1; row >= 0; row--) {
        parent = sibling;
        uint8_t niece_pos = uint8_t(shr(off.bits, unsigned(row)) & 1);
        if (n) {
            NodePtr node = n;
            if ((niece_pos & 1) == 0) {
                n = node->right;
                sibling = node->left;
            } else {
                n = node->left;
                sibling = node->right;
            }
        } else {
            sibling = nullptr;
        }
    }
    if (n && sibling && parent) return Grab{n, sibling, parent};
    return std::nullopt;
}

std::optional<Proof> MemForest::prove(const std::vector<NodeHash>& targets) const
{
    std::vector<uint64_t> positions;
    for (auto& t : targets) {
        auto it = map.find(t);
        if (it == map.end()) return std::nullopt;
        auto p = get_pos(it->second);
        if (!p) return std::nullopt;
        positions.push_back(*p);
    }
    auto needed = util::get_proof_positions(positions, leaves, util::tree_rows(leaves));
    Proof proof;
    proof.targets = positions;
    for (uint64_t pos : needed) {
        auto g = grab_node(pos);
        if (!g) return std::nullopt; // unwrap panic in Rust
        proof.hashes.push_back(g->node->data);
    }
    return proof;
}

std::optional<std::string> MemForest::del(const std::vector<NodeHash>& targets)
{
    std::vector<std::pair<uint64_t, NodeHash>> nodes;
    for (auto& t : targets) {
        auto it = map.find(t);
        if (it == map.end()) return std::string("Could not find node");
        auto pos = get_pos(it->second);
        if (!pos) return std::string("Could not get position");
        NodePtr node = it->second.lock();
        if (!node) return std::string("Could not upgrade node");
        nodes.emplace_back(*pos, node->data);
    }
    std::stable_sort(nodes.begin(), nodes.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [_, target] : nodes) {
        auto it = map.find(target);
        if (it == map.end()) return "node " + target.to_string() + " not in the forest";
        auto w = it->second;
        map.erase(it);
        NodePtr n = w.lock();
        if (!n) return std::string("dangling node"); // unwrap panic in Rust
        del_single(n);
    }
    return std::nullopt;
}

void MemForest::del_single(const NodePtr& node)
{
    if (!node->has_parent) {
        // Deleting a root
        auto it = std::find_if(roots.begin(), roots.end(), [&](const NodePtr& x) { return x->data == node->data; });
        if (it == roots.end()) return; // unwrap panic in Rust (unreachable)
        auto empty = std::make_shared<Node>();
        empty->leaf = false;
        empty->data = NodeHash::empty();
        *it = empty;
        return;
    }
    NodePtr parent = node->parent.lock();
    if (!parent) return; // `node.upgrade()?` -> None
    if (!parent->left) return; // `me.as_deref()?`
    NodePtr sibling = (parent->left->data == node->data) ? parent->right : parent->left;
    if (sibling) {
        bool gp_some = parent->has_parent;
        std::weak_ptr<Node> gpw = parent->parent;
        sibling->parent = gpw;
        sibling->has_parent = gp_some;
        NodePtr gp = gp_some ? gpw.lock() : nullptr;
        if (gp) {
            if (gp->left->data == parent->data) gp->left = sibling;
            else gp->right = sibling;
            sibling->recompute_hashes();
        } else {
            auto it = std::find_if(roots.begin(), roots.end(), [&](const NodePtr& x) { return x->data == parent->data; });
            if (it != roots.end()) *it = sibling;
        }
    }
}

void MemForest::add_single(const NodeHash& value)
{
    auto node = std::make_shared<Node>();
    node->leaf = true;
    node->data = value;
    map[value] = node;
    uint64_t l = leaves;
    while (l & 1) {
        if (roots.empty()) break; // unwrap panic in Rust (unreachable)
        NodePtr root = roots.back();
        roots.pop_back();
        if (root->data == NodeHash::empty()) {
            l >>= 1;
            continue;
        }
        auto nn = std::make_shared<Node>();
        nn->leaf = false;
        nn->data = parent_hash(root->data, node->data);
        nn->left = root;
        nn->right = node;
        root->parent = nn;
        root->has_parent = true;
        node->parent = nn;
        node->has_parent = true;
        node = nn;
        l >>= 1;
    }
    roots.push_back(node);
    leaves++;
}

std::optional<std::string> MemForest::modify(const std::vector<NodeHash>& add, const std::vector<NodeHash>& del_)
{
    // Their node always loads the forest from its database right before modifying it.
    rebuild_map();
    if (auto e = del(del_)) return e;
    for (auto& v : add) add_single(v);
    return std::nullopt;
}

} // namespace bt::utreexo
