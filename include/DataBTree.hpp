#pragma once
#include "BitmapManager.hpp"
#include "BlockManager.hpp"
#include "Singleton.hpp"
#include "macros.hpp"
#include <cstring>
#include <memory>

struct BTreeNode {
    uint64_t is_leaf;
    uint64_t key_cnt;
    uint64_t keys[BTree_M - 1];
    uint64_t ptrs[BTree_M];
    uint64_t nxt;
    BTreeNode(bool leaf = true) {
        std::memset(this, 0, BLOCK_SIZE);
        is_leaf = leaf;
    }
};
static_assert(sizeof(BTreeNode) == BLOCK_SIZE);

class DataBTree : public Singleton<DataBTree> {
    friend class Singleton;

public:
    void set_super_block(std::shared_ptr<SuperBlock> sb) { super_block = sb; }
    uint64_t find_block(uint64_t root_lba, uint64_t file_block_idx) {
        spdlog::debug("[DataBTree] 查找数据盘块");
        BTreeNode *node;
        uint64_t cur_lba = root_lba;
        std::vector<uint8_t> buffer(super_block->data.block_size);
        while (true) {
            if (cur_lba == 0)
                return 0;
            block_manager->read_block(cur_lba, buffer);
            node = reinterpret_cast<BTreeNode *>(buffer.data());
            if (node->is_leaf) {
                for (uint64_t i = 0; i < node->key_cnt; i++) {
                    if (node->keys[i] == file_block_idx)
                        return node->ptrs[i];
                }
                return 0;
            }
            uint64_t idx =
                std::distance(node->keys, std::upper_bound(node->keys, node->keys + node->key_cnt,
                                                           file_block_idx));
            cur_lba = node->ptrs[idx];
        }
    }

    // TODO::
    uint64_t insert_block(uint64_t root_lba, uint64_t file_block_idx, uint64_t file_data_lba) {
        spdlog::debug(
            "[DataBTree] 插入数据盘块. Root LBA: 0x{:X}, File Block Idx: {}, Data LBA: 0x{:X}",
            root_lba, file_block_idx, file_data_lba);
        if (root_lba == 0) {
            uint64_t new_root_lba;
            // >>>>>>>>>>>>>>>>>>>>
            spdlog::critical("???????????????????????????");
            if (!bitmap_manager->allocate_block(new_root_lba)) {
                return 0;
            }
            auto node = std::make_unique<BTreeNode>(true);
            node->key_cnt = 1;
            node->keys[0] = file_block_idx;
            node->ptrs[0] = file_data_lba;
            write_node(new_root_lba, node.get());
            return new_root_lba;
        }

        std::vector<uint8_t> buffer(super_block->data.block_size);
        block_manager->read_block(root_lba, buffer);
        BTreeNode *root_node = reinterpret_cast<BTreeNode *>(buffer.data());

        if (root_node->key_cnt == super_block->data.BTree_M - 1) {
            uint64_t new_root_lba;
            if (!bitmap_manager->allocate_block(new_root_lba)) {
                return 0;
            }

            BTreeNode new_root(false);
            new_root.key_cnt = 0;
            new_root.ptrs[0] = root_lba;
            write_node(new_root_lba, &new_root);
            if (!split_child(new_root_lba, 0)) {
                return 0;
            }
            root_lba = new_root_lba;
        }

        node_insert(root_lba, file_block_idx, file_data_lba);
        return root_lba;
    }

private:
    void write_node(uint64_t lba, BTreeNode *node) {
        block_manager->write_block(lba, std::span<uint8_t>(reinterpret_cast<uint8_t *>(node),
                                                           super_block->data.block_size));
    }

    bool split_child(uint64_t father_node_lba, uint64_t child_idx) {
        uint64_t new_node_lba;
        if (!bitmap_manager->allocate_block(new_node_lba)) {
            return false;
        }

        std::vector<uint8_t> father_node_buffer(super_block->data.block_size);
        block_manager->read_block(father_node_lba, father_node_buffer);
        BTreeNode *father_node = reinterpret_cast<BTreeNode *>(father_node_buffer.data());

        std::vector<uint8_t> child_node_buffer(super_block->data.block_size);
        uint64_t child_lba = father_node->ptrs[child_idx];
        block_manager->read_block(child_lba, child_node_buffer);
        BTreeNode *child_node = reinterpret_cast<BTreeNode *>(child_node_buffer.data());

        auto new_node = std::make_unique<BTreeNode>(child_node->is_leaf);
        uint64_t mid = (super_block->data.BTree_M - 1) >> 1;

        if (child_node->is_leaf) {
            new_node->key_cnt = super_block->data.BTree_M - 1 - mid;
            std::memcpy(new_node->keys, child_node->keys + mid,
                        new_node->key_cnt * sizeof(uint64_t));
            std::memcpy(new_node->ptrs, child_node->ptrs + mid,
                        new_node->key_cnt * sizeof(uint64_t));

            child_node->nxt = new_node_lba;
        } else {
            new_node->key_cnt = super_block->data.BTree_M - 1 - mid - 1;
            std::memcpy(new_node->keys, child_node->keys + mid + 1,
                        new_node->key_cnt * sizeof(uint64_t));
            std::memcpy(new_node->ptrs, child_node->ptrs + mid + 1,
                        (new_node->key_cnt + 1) * sizeof(uint64_t));
        }
        child_node->key_cnt = mid;
        uint64_t insert_idx =
            std::upper_bound(father_node->keys, father_node->keys + father_node->key_cnt,
                             child_node->keys[mid]) -
            father_node->keys;

        father_node->key_cnt++;
        for (uint64_t i = father_node->key_cnt - 1; i > insert_idx; i--)
            father_node->keys[i] = father_node->keys[i - 1];
        for (uint64_t i = father_node->key_cnt; i > insert_idx + 1; i--)
            father_node->ptrs[i] = father_node->ptrs[i - 1];
        father_node->keys[insert_idx] = child_node->keys[mid];
        father_node->ptrs[insert_idx + 1] = new_node_lba;

        write_node(father_node_lba, father_node);
        write_node(child_lba, child_node);
        write_node(new_node_lba, new_node.get());

        return true;
    }

    bool node_insert(uint64_t node_lba, uint64_t key, uint64_t data_lba) {
        std::vector<uint8_t> buffer(super_block->data.block_size);
        block_manager->read_block(node_lba, buffer);
        BTreeNode *node = reinterpret_cast<BTreeNode *>(buffer.data());

        if (node->is_leaf) {
            uint64_t insert_idx =
                std::upper_bound(node->keys, node->keys + node->key_cnt, key) - node->keys;
            for (uint64_t i = node->key_cnt; i > insert_idx; i--) {
                node->keys[i] = node->keys[i - 1];
                node->ptrs[i] = node->ptrs[i - 1];
            }
            node->key_cnt++;
            node->keys[insert_idx] = key;
            node->ptrs[insert_idx] = data_lba;
            write_node(node_lba, node);
            return true;
        }

        uint64_t idx = std::upper_bound(node->keys, node->keys + node->key_cnt, key) - node->keys;
        uint64_t child_lba = node->ptrs[idx];
        std::vector<uint8_t> child_buffer(BLOCK_SIZE);
        block_manager->read_block(child_lba, child_buffer);
        BTreeNode *child_node = reinterpret_cast<BTreeNode *>(child_buffer.data());

        if (child_node->key_cnt == super_block->data.BTree_M - 1) {
            if (!split_child(node_lba, idx))
                return false;
            block_manager->read_block(node_lba, buffer);
            node = reinterpret_cast<BTreeNode *>(buffer.data());
            if (key > node->keys[idx])
                idx++;
            child_lba = node->ptrs[idx];
        }

        buffer.clear();
        child_buffer.clear();
        return node_insert(child_lba, key, data_lba);
    }

private:
    std::shared_ptr<SuperBlock> super_block;
    BlockManager *block_manager = BlockManager::get_instance();
    BitmapManager *bitmap_manager = BitmapManager::get_instance();
};
