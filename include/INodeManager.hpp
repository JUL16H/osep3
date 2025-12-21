#pragma once
#include "BitmapManager.hpp"
#include "BlockManager.hpp"
#include "INode.hpp"

class INodeManager {
public:
    INodeManager(SuperBlock &sb, BlockManager &_block_manager, BitmapManager &_bitmap_manager)
        : super_block(sb), bitmap_manager(_bitmap_manager), block_manager(_block_manager) {}

    void reset_inode_bitmap() {
        spdlog::debug("[INodeManager] 写入INode位图");
        std::vector<uint8_t> buffer(super_block.data.block_size, 0);
        for (uint64_t i = 0; i < super_block.data.inode_valid_blocks_cnt; i++) {
            block_manager.write_block(i + super_block.data.inode_valid_block_start_lba, buffer);
        }
        spdlog::debug("[INodeManager] INode位图写入完成");
    }

    bool allocate_inode(uint64_t &id) {
        spdlog::debug("[INodeManager] 查找空闲INode.");
        bool find = false;
        uint64_t bitmap_block_idx, byte_idx;
        uint8_t bit_idx;
        std::vector<uint8_t> buffer(super_block.data.block_size);

        for (bitmap_block_idx = 0; bitmap_block_idx < super_block.data.inode_valid_blocks_cnt;
             bitmap_block_idx++) {
            block_manager.read_block(
                bitmap_block_idx + super_block.data.inode_valid_block_start_lba, buffer);
            for (byte_idx = 0; byte_idx < super_block.data.block_size; byte_idx++) {
                if (buffer[byte_idx] != 0xff) {
                    find = true;
                    for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                        if (~buffer[byte_idx] & (1 << (7 - bit_idx)))
                            break;
                    }
                }
                if (find)
                    break;
            }
            if (find)
                break;
        }
        if (!find) {
            spdlog::warn("[INodeManager] 未找到空闲INode.");
            return false;
        }

        buffer[byte_idx] |= (1 << (7 - bit_idx));
        block_manager.write_block(bitmap_block_idx + super_block.data.inode_valid_block_start_lba,
                                  buffer);
        id = bitmap_block_idx * super_block.data.bits_per_block + byte_idx * 8 + bit_idx;
        spdlog::debug("[INodeManager] 找到空闲INode, id: {}", id);
        return true;
    }

    bool inode_add(uint64_t inode_id, std::span<uint8_t> data) {
        spdlog::info("[INodeManager] 向文件写入数据.");
        spdlog::debug("[INodeManager] INode ID: {:X}.", inode_id);

        spdlog::debug("[INodeManager] 从硬盘中读取INode.");
        std::vector<uint8_t> buffer(super_block.data.block_size);
        uint32_t dir_inode_block_lba =
            inode_id / super_block.data.inodes_per_block + super_block.data.inode_block_start_lba;
        INode inode;
        block_manager.read_block(dir_inode_block_lba, buffer);
        std::memcpy(&inode,
                    buffer.data() + (inode_id % super_block.data.inodes_per_block) *
                                        super_block.data.inode_size,
                    sizeof(INode));

        bool success = false;

        // TODO:
        switch (inode.storage_type) {
        case StorageType::Inline:
            if (inode.size + data.size() <= super_block.data.inode_data_size) {
                std::memcpy(inode.inline_data + inode.size, data.data(), data.size());
                success = true;
            } else {
                // FIX: 追加大量数据可能导致跳过Direct直接使用BTree
                uint64_t block_lba;
                if (!bitmap_manager.allocate_block(block_lba))
                    return false;
                inode.storage_type = StorageType::Direct;
                inode.block_lba = block_lba;
                std::ranges::fill(buffer, 0);
                std::memcpy(buffer.data(), inode.inline_data, inode.size);
                std::memset(inode.inline_data, 0, inode.size);
                std::memcpy(buffer.data() + inode.size, data.data(), data.size());
                block_manager.write_block(block_lba, buffer);
                success = true;
            }
            break;

        case StorageType::Direct:
            if (inode.size + data.size() <= super_block.data.block_size) {
                block_manager.read_block(inode.block_lba, buffer);
                std::memcpy(buffer.data() + inode.size, data.data(), data.size());
                block_manager.write_block(inode.block_lba, buffer);
                success = true;
                // TODO:
            } else {
            }
            break;

        case StorageType::BTree:
            break;
        }

        if (success)
            inode.size += data.size();

        spdlog::debug("[INodeManager] INode写回硬盘.");
        std::memcpy(buffer.data() + (inode_id % super_block.data.inodes_per_block) *
                                        super_block.data.inode_size,
                    &inode, sizeof(INode));
        block_manager.write_block(dir_inode_block_lba, buffer);

        return false;
    }

private:
    const SuperBlock &super_block;
    BlockManager &block_manager;
    BitmapManager &bitmap_manager;
};
