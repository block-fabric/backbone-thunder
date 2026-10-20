// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Utreexo accumulator, behaviour-exact with rustreexo rev 6da503b (MIT, mit-dci) as used by
// thunder-rust 0.18: MemForest (modify/prove/verify/serialize) and Proof::verify.
//
// The port is deliberately literal, including the parts that look like bugs, because the
// resulting roots are committed in every block header:
//  * nodes are located by comparing HASH VALUES (not identities) when walking parents,
//    choosing siblings and finding roots, so duplicate leaves behave as they do in Rust;
//  * the leaf index is a hash map rebuilt the way MemForest::deserialize rebuilds it
//    (pre-order traversal, last insert wins), because their node reloads the forest
//    from its database before every modification;
//  * deleting a root leaf leaves an Empty root, which add() later discards;
//  * Proof::verify accepts a proof with no targets, and accepts when the number of
//    unmatched computed roots equals the number of matched ones;
//  * integer arithmetic follows Rust release semantics (wrapping, shift amounts masked).
#pragma once

#include <bt/bytes.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bt::utreexo {

/** rustreexo BitcoinNodeHash = enum { Empty, Placeholder, Some([u8;32]) }, derived Ord/Eq. */
struct NodeHash {
    enum Tag : uint8_t { Empty = 0, Placeholder = 1, Some = 2 };
    Tag tag = Empty;
    Hash32 h{};

    static NodeHash some(const Hash32& x) { return NodeHash{Some, x}; }
    static NodeHash empty() { return NodeHash{}; }
    bool is_empty() const { return tag == Empty; }
    /** Deref: Some -> bytes, otherwise 32 zero bytes. */
    const Hash32& bytes() const;
    friend auto operator<=>(const NodeHash& a, const NodeHash& b)
    {
        if (auto c = a.tag <=> b.tag; c != 0) return c;
        if (a.tag != Some) return std::strong_ordering::equal;
        return a.h <=> b.h;
    }
    friend bool operator==(const NodeHash& a, const NodeHash& b) { return (a <=> b) == 0; }
    std::string to_string() const; // Display: hex, or "empty"
};

struct NodeHashHasher {
    size_t operator()(const NodeHash& n) const
    {
        size_t r = n.tag;
        for (int i = 0; i < 8; i++) r = r * 131 + n.h[i];
        return r;
    }
};

/** sha512_256(Deref(left) || Deref(right)) */
NodeHash parent_hash(const NodeHash& l, const NodeHash& r);

struct Proof {
    std::vector<uint64_t> targets;
    std::vector<NodeHash> hashes;

    /** Proof::verify(del_hashes, roots, num_leaves). Errors are returned as std::nullopt. */
    std::optional<bool> verify(const std::vector<NodeHash>& del_hashes, const std::vector<NodeHash>& roots,
                               uint64_t num_leaves) const;
    bool operator==(const Proof&) const = default;
};

namespace util {
uint8_t tree_rows(uint64_t n);
uint8_t detect_row(uint64_t pos, uint8_t forest_rows);
uint64_t root_position(uint64_t num_leaves, uint8_t row, uint8_t forest_rows);
bool is_root_position(uint64_t position, uint64_t num_leaves, uint8_t forest_rows);
uint64_t parent(uint64_t pos, uint8_t forest_rows);
uint64_t left_child(uint64_t pos, uint8_t forest_rows);
uint64_t right_child(uint64_t pos, uint8_t forest_rows);
std::vector<uint64_t> get_proof_positions(const std::vector<uint64_t>& targets, uint64_t num_leaves, uint8_t forest_rows);
struct Offset {
    uint8_t tree, branch_len;
    uint64_t bits;
};
Offset detect_offset(uint64_t pos, uint64_t num_leaves);
} // namespace util

class MemForest {
public:
    struct Node;
    using NodePtr = std::shared_ptr<Node>;
    struct Node {
        bool leaf = false;
        NodeHash data;
        std::weak_ptr<Node> parent; // empty weak == None
        bool has_parent = false;    // distinguishes Some(dead weak) from None
        NodePtr left, right;
        void recompute_hashes();
    };

    MemForest() = default;
    MemForest(const MemForest& o) { *this = o; }
    MemForest& operator=(const MemForest& o);
    MemForest(MemForest&&) = default;
    MemForest& operator=(MemForest&&) = default;

    uint64_t leaves = 0;

    /** del(del) then add(add). Returns an error string on failure (their Err(String)). */
    std::optional<std::string> modify(const std::vector<NodeHash>& add, const std::vector<NodeHash>& del);
    std::vector<NodeHash> get_roots() const;
    std::optional<bool> verify(const Proof& proof, const std::vector<NodeHash>& del_hashes) const
    {
        return proof.verify(del_hashes, get_roots(), leaves);
    }
    /** MemForest::prove; std::nullopt where theirs returns Err. */
    std::optional<Proof> prove(const std::vector<NodeHash>& targets) const;

    /** rustreexo MemForest::serialize / deserialize byte format. */
    Bytes serialize() const;
    static std::optional<MemForest> deserialize(ByteSpan b);

    /** Rebuild the leaf map exactly as deserialize() would (pre-order, last wins). */
    void rebuild_map();

private:
    std::vector<NodePtr> roots;
    std::unordered_map<NodeHash, std::weak_ptr<Node>, NodeHashHasher> map;

    std::optional<std::string> del(const std::vector<NodeHash>& targets);
    void del_single(const NodePtr& node);
    void add_single(const NodeHash& value);
    std::optional<uint64_t> get_pos(const std::weak_ptr<Node>& node) const;
    struct Grab {
        NodePtr node, sibling, parent;
    };
    std::optional<Grab> grab_node(uint64_t pos) const;
};

} // namespace bt::utreexo
