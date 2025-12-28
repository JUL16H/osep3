#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

template <typename Key, typename NodeID>
struct IBPTreeStorage {
    ~IBPTreeStorage() = default;

    virtual void read_node(NodeID id, std::span<uint8_t> buffer) = 0;
    virtual void write_node(NodeID id, std::span<uint8_t> data) = 0;
    virtual std::optional<NodeID> allocate_node() = 0;
    virtual void free_node(NodeID id) = 0;
    virtual void free_val(Key key) = 0;
    virtual size_t get_node_size() const = 0;
};

template <typename Key, typename Val, size_t Blocksize>
struct BPTreeNode {
    static constexpr size_t HeaderSize = 16 + sizeof(Val);
    static constexpr size_t PairSize = sizeof(Key) + sizeof(Val);
    static constexpr size_t M = (Blocksize - HeaderSize) / PairSize;

    uint64_t is_leaf;
    uint64_t key_cnt;
    Val nxt;

    Key keys[M];
    Val vals[M];
    char padding[Blocksize - M * PairSize - HeaderSize];

    BPTreeNode() { std::memset(this, 0, Blocksize); }
};

template <typename Key, typename Val, size_t Blocksize>
class BPTree {
public:
    using Node = BPTreeNode<Key, Val, Blocksize>;
    using Storage = IBPTreeStorage<Key, Val>;

    BPTree(std::shared_ptr<Storage> _storage) : storage(_storage) {}

    std::optional<uint64_t> insert(Val root_id, Key key, Val val) {
        if (root_id == 0) {
            auto new_root_id = storage->allocate_node();
            if (!new_root_id) {
                return std::nullopt;
            }
            auto node = std::make_unique<Node>();
            node->is_leaf = true;
            node->key_cnt = 1;
            node->keys[0] = key;
            node->vals[0] = val;
            storage->write_node(new_root_id.value(), to_span(node.get()));
            return new_root_id.value();
        }

        auto root_node_ptr = std::make_unique<Node>();
        storage->read_node(root_id, to_span(root_node_ptr.get()));
        Node *root_node = root_node_ptr.get();

        if (root_node->key_cnt == Node::M - 1) {
            auto new_root_id = storage->allocate_node();
            if (!new_root_id) {
                return std::nullopt;
            }

            auto new_root_ptr = std::make_unique<Node>();
            new_root_ptr->is_leaf = false;
            new_root_ptr->key_cnt = 0;
            new_root_ptr->vals[0] = root_id;
            storage->write_node(new_root_id.value(), to_span(new_root_ptr.get()));

            if (!split_node(new_root_id.value(), 0)) {
                return std::nullopt;
            }
            root_id = new_root_id.value();
        }

        node_insert(root_id, key, val);
        return root_id;
    }

    std::optional<Val> find(Val root_id, Key key) {
        if (root_id == 0)
            return std::nullopt;

        auto node_ptr = std::make_unique<Node>();
        Val cur_id = root_id;
        Node *node = nullptr;

        while (true) {
            storage->read_node(cur_id, to_span(node_ptr.get()));
            node = node_ptr.get();

            if (node->is_leaf)
                break;

            uint64_t idx = std::distance(
                node->keys, std::upper_bound(node->keys, node->keys + node->key_cnt, key));
            cur_id = node->vals[idx];
        }

        auto it = std::lower_bound(node->keys, node->keys + node->key_cnt, key);
        uint64_t idx = std::distance(node->keys, it);

        if (idx < node->key_cnt && node->keys[idx] == key) {
            return node->vals[idx];
        }
        return std::nullopt;
    }

    void clear(Val id) {
        if (id == 0)
            return;
        auto node_ptr = std::make_unique<Node>();
        storage->read_node(id, to_span(node_ptr.get()));
        Node *node = node_ptr.get();

        if (!node->is_leaf) {
            for (uint64_t i = 0; i <= node->key_cnt; i++) {
                clear(node->vals[i]);
            }
        } else {
            for (uint64_t i = 0; i < node->key_cnt; i++)
                storage->free_val(node->vals[i]);
        }
        storage->free_node(id);
    }

    std::optional<Key> get_min_key(Val id) {
        std::vector<uint8_t> buffer(Blocksize);
        storage->read_node(id, buffer);
        Node *node = reinterpret_cast<Node *>(buffer.data());
        if (node->key_cnt == 0)
            return std::nullopt;

        if (node->is_leaf) {
            return node->keys[0];
        } else {
            return get_min_key(node->vals[0]);
        }
    }

private:
    std::span<uint8_t> to_span(Node *node) {
        return std::span<uint8_t>(reinterpret_cast<uint8_t *>(node), Blocksize);
    }

    bool split_node(Val father_id, uint64_t child_idx) {
        auto newidopt = storage->allocate_node();
        if (!newidopt)
            return false;

        auto father_node_ptr = std::make_unique<Node>();
        storage->read_node(father_id, to_span(father_node_ptr.get()));
        Node *father_node = father_node_ptr.get();

        Val node_id = father_node->vals[child_idx];

        auto node_ptr = std::make_unique<Node>();
        storage->read_node(node_id, to_span(node_ptr.get()));
        Node *node = node_ptr.get();

        Node new_node;
        uint64_t mid = (Node::M - 1) >> 1;

        if (node->is_leaf) {
            new_node.is_leaf = true;
            new_node.key_cnt = Node::M - 1 - mid;
            std::memcpy(new_node.keys, node->keys + mid, new_node.key_cnt * sizeof(Key));
            std::memcpy(new_node.vals, node->vals + mid, new_node.key_cnt * sizeof(Val));

            new_node.nxt = node->nxt;
            node->nxt = newidopt.value();
        } else {
            new_node.is_leaf = false;
            new_node.key_cnt = Node::M - 1 - mid - 1;
            std::memcpy(new_node.keys, node->keys + mid + 1, new_node.key_cnt * sizeof(Key));
            std::memcpy(new_node.vals, node->vals + mid + 1, (new_node.key_cnt + 1) * sizeof(Val));
        }
        node->key_cnt = mid;
        uint64_t insert_idx = child_idx;

        for (uint64_t i = father_node->key_cnt + 1; i > insert_idx + 1; i--)
            father_node->vals[i] = father_node->vals[i - 1];

        father_node->key_cnt++;
        for (uint64_t i = father_node->key_cnt; i > insert_idx; i--)
            father_node->keys[i] = father_node->keys[i - 1];

        father_node->keys[insert_idx] = node->keys[mid];
        father_node->vals[insert_idx + 1] = newidopt.value();

        storage->write_node(father_id, to_span(father_node));
        storage->write_node(node_id, to_span(node));
        storage->write_node(newidopt.value(), to_span(&new_node));

        return true;
    }

    bool node_insert(Val id, Key key, Val val) {
        auto node_ptr = std::make_unique<Node>();
        storage->read_node(id, to_span(node_ptr.get()));
        Node *node = node_ptr.get();

        if (node->is_leaf) {
            uint64_t insert_idx = std::distance(
                node->keys, std::upper_bound(node->keys, node->keys + node->key_cnt, key));
            for (uint64_t i = node->key_cnt; i > insert_idx; i--) {
                node->keys[i] = node->keys[i - 1];
                node->vals[i] = node->vals[i - 1];
            }
            node->key_cnt++;
            node->keys[insert_idx] = key;
            node->vals[insert_idx] = val;
            storage->write_node(id, to_span(node));
            return true;
        }

        uint64_t idx = std::distance(node->keys,
                                     std::upper_bound(node->keys, node->keys + node->key_cnt, key));
        Val child_id = node->vals[idx];

        auto child_node_ptr = std::make_unique<Node>();
        storage->read_node(child_id, to_span(child_node_ptr.get()));
        Node *child_node = child_node_ptr.get();

        if (child_node->key_cnt == Node::M - 1) {
            if (!split_node(id, idx))
                return false;

            storage->read_node(id, to_span(node_ptr.get()));
            node = node_ptr.get(); // update pointer just in case

            if (key >= node->keys[idx])
                idx++;
            child_id = node->vals[idx];
        }

        return node_insert(child_id, key, val);
    }

private:
    std::shared_ptr<Storage> storage;
};
