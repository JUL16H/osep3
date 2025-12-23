#pragma once
#include "BitmapManager.hpp"
#include "BlockManager.hpp"
#include "DataBTree.hpp"
#include "INode.hpp"

// TODO: 添加缓存
class INodeManager : public Singleton<INodeManager> {
    friend class Singleton;

public:
    void set_super_block(std::shared_ptr<SuperBlock> sb) { super_block = sb; }

    void reset_inode_bitmap() {
        spdlog::debug("[INodeManager] 写入INode位图");
        std::vector<uint8_t> buffer(super_block->data.block_size, 0);
        for (uint64_t i = 0; i < super_block->data.inode_valid_blocks_cnt; i++) {
            block_manager->write_block(i + super_block->data.inode_valid_block_start_lba, buffer);
        }
        spdlog::debug("[INodeManager] INode位图写入完成");
    }

    bool allocate_inode(uint64_t &id) {
        spdlog::debug("[INodeManager] 查找空闲INode.");
        bool find = false;
        uint64_t bitmap_block_idx, byte_idx;
        uint8_t bit_idx;
        std::vector<uint8_t> buffer(super_block->data.block_size);

        for (bitmap_block_idx = 0; bitmap_block_idx < super_block->data.inode_valid_blocks_cnt;
             bitmap_block_idx++) {
            block_manager->read_block(
                bitmap_block_idx + super_block->data.inode_valid_block_start_lba, buffer);
            for (byte_idx = 0; byte_idx < super_block->data.block_size; byte_idx++) {
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
        block_manager->write_block(bitmap_block_idx + super_block->data.inode_valid_block_start_lba,
                                   buffer);
        id = bitmap_block_idx * super_block->data.bits_per_block + byte_idx * 8 + bit_idx;
        spdlog::debug("[INodeManager] 找到空闲INode, id: {}", id);

        super_block->data.free_inodes--;
        block_manager->refresh_super_block();

        return true;
    }

    // HACK:
    bool inode_add_data(uint64_t inode_id, std::span<uint8_t> data) {
        spdlog::info("[INodeManager] 向文件写入数据.");
        spdlog::debug("[INodeManager] INode ID: {:X}, 写入数据大小: {} B.", inode_id, data.size());

        uint64_t dir_inode_block_lba =
            super_block->data.inode_block_start_lba + inode_id / super_block->data.inodes_per_block;

        uint64_t inode_offset =
            (inode_id % super_block->data.inodes_per_block) * super_block->data.inode_size;

        std::vector<uint8_t> buffer(super_block->data.block_size);
        block_manager->read_block(dir_inode_block_lba, buffer);

        INode inode;
        std::memcpy(&inode, buffer.data() + inode_offset, sizeof(INode));

        if (inode.storage_type == StorageType::Inline) {
            if (inode.size + data.size() <= super_block->data.inode_data_size) {
                std::memcpy(inode.inline_data + inode.size, data.data(), data.size());
                inode.size += data.size();
                data = data.subspan(data.size()); // 数据已全部写入
            }
            else {
                spdlog::debug("[INodeManager] Inline 空间不足，升级为 Direct.");
                uint64_t block_lba;
                if (!bitmap_manager->allocate_block(block_lba)) {
                    spdlog::error("[INodeManager] 无法分配 Direct 块.");
                    return false;
                }

                inode.storage_type = StorageType::Direct;
                inode.block_lba = block_lba;

                std::vector<uint8_t> data_buffer(super_block->data.block_size, 0);
                std::memcpy(data_buffer.data(), inode.inline_data, inode.size);

                std::memset(inode.inline_data, 0, INODE_DATA_SIZE);

                uint64_t space_left = super_block->data.block_size - inode.size;
                uint64_t to_write = std::min((uint64_t)data.size(), space_left);

                std::memcpy(data_buffer.data() + inode.size, data.data(), to_write);
                block_manager->write_block(block_lba, data_buffer);

                inode.size += to_write;
                data = data.subspan(to_write);
            }
        }

        if (inode.storage_type == StorageType::Direct && !data.empty()) {
            if (inode.size < super_block->data.block_size) {
                std::vector<uint8_t> data_buffer(super_block->data.block_size);
                block_manager->read_block(inode.block_lba, data_buffer);

                uint64_t space_left = super_block->data.block_size - inode.size;
                uint64_t to_write = std::min((uint64_t)data.size(), space_left);

                std::memcpy(data_buffer.data() + inode.size, data.data(), to_write);
                block_manager->write_block(inode.block_lba, data_buffer);

                inode.size += to_write;
                data = data.subspan(to_write);
            }

            if (!data.empty()) {
                spdlog::debug("[INodeManager] Direct 空间不足，升级为 BTree.");
                inode.storage_type = StorageType::BTree;

                uint64_t new_root_lba = data_btree->insert_block(0, 0, inode.block_lba);
                if (new_root_lba == 0) {
                    spdlog::error("[INodeManager] BTree 根节点创建失败.");
                    return false;
                }
                inode.block_lba = new_root_lba;
            }
        }

        if (inode.storage_type == StorageType::BTree && !data.empty()) {
            uint64_t last_block_offset = inode.size % super_block->data.block_size;
            if (last_block_offset != 0) {
                uint64_t block_idx = inode.size / super_block->data.block_size;
                uint64_t last_data_lba = data_btree->find_block(inode.block_lba, block_idx);

                if (last_data_lba == 0) {
                    spdlog::error("[INodeManager] 索引错误：无法找到尾部数据块.");
                    return false;
                }

                std::vector<uint8_t> data_buffer(super_block->data.block_size);
                block_manager->read_block(last_data_lba, data_buffer);

                uint64_t space_left = super_block->data.block_size - last_block_offset;
                uint64_t to_write = std::min((uint64_t)data.size(), space_left);

                std::memcpy(data_buffer.data() + last_block_offset, data.data(), to_write);
                block_manager->write_block(last_data_lba, data_buffer);

                inode.size += to_write;
                data = data.subspan(to_write);
            }

            std::vector<uint8_t> data_buffer(super_block->data.block_size);
            while (!data.empty()) {
                uint64_t new_data_lba;
                if (!bitmap_manager->allocate_block(new_data_lba)) {
                    spdlog::error("[INodeManager] 磁盘已满，无法分配新块.");
                    return false;
                }

                uint64_t to_write =
                    std::min((uint64_t)data.size(), (uint64_t)super_block->data.block_size);

                std::memset(data_buffer.data(), 0, super_block->data.block_size);
                std::memcpy(data_buffer.data(), data.data(), to_write);
                block_manager->write_block(new_data_lba, data_buffer);

                uint64_t block_idx = inode.size / super_block->data.block_size;
                uint64_t new_root =
                    data_btree->insert_block(inode.block_lba, block_idx, new_data_lba);

                if (new_root == 0) {
                    spdlog::error("[INodeManager] BTree 插入节点失败.");
                    return false;
                }

                inode.block_lba = new_root;
                inode.size += to_write;
                data = data.subspan(to_write);
            }
        }

        std::memcpy(buffer.data() + inode_offset, &inode, sizeof(INode));
        block_manager->write_block(dir_inode_block_lba, buffer);

        spdlog::debug("[INodeManager] 数据写入完成，新文件大小: {} B.", inode.size);
        return true;
    }

    // TODO:判断INode是否存在
    bool get_inode_info(uint64_t id, INode &inode) {
        spdlog::debug("[INodeManager] 获取INode, ID: {}", id);
        std::vector<uint8_t> buffer(super_block->data.block_size);
        uint64_t lba =
            super_block->data.inode_block_start_lba + id / super_block->data.inodes_per_block;
        uint64_t off = (id % super_block->data.inodes_per_block) * super_block->data.inode_size;
        block_manager->read_block(lba, buffer);
        std::memcpy(&inode, buffer.data() + off, super_block->data.inode_size);
        return true;
    }

    bool get_inode_data_block(uint64_t id, uint64_t block_idx, std::span<uint8_t> buffer) {
        if (buffer.size() != super_block->data.block_size) {
            throw std::runtime_error("buffer大小与盘块大小不符.");
        }

        INode inode;

        // TODO:
        if (!get_inode_info(id, inode)) {
            return false;
        }

        if (block_idx >=
            (inode.size + super_block->data.block_size - 1) / super_block->data.block_size) {
            throw std::runtime_error("尝试获取超过INode大小的信息.");
        }

        switch (inode.storage_type) {
        case StorageType::Inline:
            std::memcpy(buffer.data(), inode.inline_data, inode.size);
            break;
        case StorageType::Direct:
            block_manager->read_block(inode.block_lba, buffer);
            break;
        // TODO:
        case StorageType::BTree:
            uint64_t lba = data_btree->find_block(inode.block_lba, block_idx);
            if (!lba) {
                return false;
            }
            block_manager->read_block(lba, buffer);
            break;
        }
        return true;
    }

private:
    std::shared_ptr<SuperBlock> super_block;
    BlockManager *block_manager = BlockManager::get_instance();
    BitmapManager *bitmap_manager = BitmapManager::get_instance();
    DataBTree *data_btree = DataBTree::get_instance();
};
