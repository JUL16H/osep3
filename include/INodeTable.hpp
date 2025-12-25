#pragma once
#include "BlockAllocator.hpp"
#include "BlockIndexer.hpp"
#include "INode.hpp"
#include "IOContext.hpp"

struct DirItem {
    uint64_t inode_id;
    uint8_t valid;
    char name[FILENAME_SIZE];
    DirItem(uint64_t _inode_id = 0, std::string _name = "") {
        std::memset(this, 0, DIRITEM_SIZE);
        inode_id = _inode_id;
        valid = 1;
        std::memcpy(name, _name.data(), std::min((size_t)FILENAME_SIZE, _name.size()));
    }
};
static_assert(sizeof(DirItem) == DIRITEM_SIZE);

// TODO: 添加缓存
class INodeTable {
public:
    INodeTable(std::shared_ptr<SuperBlock> _sb, std::shared_ptr<IOContext> _ioc,
               std::shared_ptr<BlockAllocator> _blkalloc, std::shared_ptr<BlockIndexer> _blkidxer)
        : sb(_sb), iocontext(_ioc), blkalloc(_blkalloc), blkidxer(_blkidxer) {}

    void reset_inode_bitmap() {
        spdlog::debug("[INodeManager] 写入INode位图");
        std::vector<uint8_t> buffer(sb->data.block_size, 0);
        for (uint64_t i = 0; i < sb->data.inode_valid_blocks_cnt; i++) {
            iocontext->write_block(i + sb->data.inode_valid_block_start_lba, buffer);
        }
        spdlog::debug("[INodeManager] INode位图写入完成");
    }

    bool allocate_inode(uint64_t &id) {
        spdlog::debug("[INodeManager] 查找空闲INode.");
        bool find = false;
        uint64_t bitmap_block_idx, byte_idx;
        uint8_t bit_idx;
        std::vector<uint8_t> buffer(sb->data.block_size);

        for (bitmap_block_idx = 0; bitmap_block_idx < sb->data.inode_valid_blocks_cnt;
             bitmap_block_idx++) {
            iocontext->read_block(bitmap_block_idx + sb->data.inode_valid_block_start_lba, buffer);
            for (byte_idx = 0; byte_idx < sb->data.block_size; byte_idx++) {
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
        iocontext->write_block(bitmap_block_idx + sb->data.inode_valid_block_start_lba, buffer);
        id = bitmap_block_idx * sb->data.bits_per_block + byte_idx * 8 + bit_idx;
        spdlog::debug("[INodeManager] 找到空闲INode, id: {}", id);

        sb->data.free_inodes--;
        iocontext->refresh_super_block();

        return true;
    }

    std::vector<uint8_t> read_data(uint64_t id, uint64_t offset, uint64_t max_size) {
        INode node;
        get_inode(id, &node);

        uint64_t size = std::min(max_size, node.size - offset);
        std::vector<uint8_t> data(size, 0);

        if (node.storage_type == StorageType::Inline) {
            std::memcpy(data.data(), node.inline_data, size);
        } else if (node.storage_type == StorageType::Direct) {
            std::vector<uint8_t> data_block_buffer(sb->data.block_size);
            iocontext->read_block(node.block_lba, data_block_buffer);
            std::memcpy(data.data(), data_block_buffer.data(), size);
        } else if (node.storage_type == StorageType::Index) {
            std::vector<uint64_t> data_block_lbas =
                blkidxer->find_blocks(node.block_lba, offset / sb->data.block_size,
                                      (offset + size - 1) / sb->data.block_size);
            offset %= sb->data.block_size;
            std::vector<uint8_t> data_block_buffer(sb->data.block_size);
            for (uint64_t i = 0; i < data_block_lbas.size(); i++) {
                if (data_block_lbas[i] == 0) {
                    size -= sb->data.block_size;
                    continue;
                }
                uint64_t cur_epoch_size = std::min(size, sb->data.block_size - offset);
                iocontext->read_block(data_block_lbas[i], data_block_buffer);
                std::memcpy(data.data() + offset, data_block_buffer.data(), cur_epoch_size);
                offset = 0;
                size -= cur_epoch_size;
            }
        }
        return data;
    }

    bool write_data(uint64_t inode_id, uint64_t offset, std::span<uint8_t> data) {
        if (data.empty())
            return true;
        INode inode;
        get_inode(inode_id, &inode);

        if (inode.storage_type == StorageType::Inline) {
            if (offset + data.size() <= sb->data.inode_inline_data_size) {
                std::memcpy(inode.inline_data + offset, data.data(), data.size());
                inode.size = std::max(inode.size, offset + data.size());
                write_inode_to_disk(inode_id, &inode);
                return true;
            }
            // 数据超出 Inline 范围
            uint64_t data_block_lba;
            if (!blkalloc->allocate_block(data_block_lba)) {
                return false;
            }
            std::vector<uint8_t> data_buffer(sb->data.block_size);
            std::memcpy(data_buffer.data(), inode.inline_data, inode.size);
            std::memset(inode.inline_data, 0, inode.size);
            if (offset <= sb->data.block_size) {
                const uint64_t cur_block_new_data_size =
                    std::min(data.size() + offset, (size_t)sb->data.block_size) - offset;
                std::memcpy(data_buffer.data() + offset, data.data(), cur_block_new_data_size);
                inode.size = std::max(inode.size, offset + cur_block_new_data_size);
                offset += cur_block_new_data_size;
                data = data.subspan(cur_block_new_data_size);
            }
            iocontext->write_block(data_block_lba, data_buffer);
            inode.block_lba = data_block_lba;
            inode.storage_type = StorageType::Direct;
            if (data.empty()) {
                write_inode_to_disk(inode_id, &inode);
                return true;
            }
        }
        if (inode.storage_type == StorageType::Direct) {
            if (data.size() + offset <= sb->data.block_size) {
                std::vector<uint8_t> data_buffer(sb->data.block_size);
                iocontext->read_block(inode.block_lba, data_buffer);
                std::memcpy(data_buffer.data() + offset, data.data(), data.size());
                iocontext->write_block(inode.block_lba, data_buffer);
                inode.size = std::max(inode.size, offset + data.size());
                write_inode_to_disk(inode_id, &inode);
                return true;
            }
            // 数据超出 Direct 范围
            if (offset <= sb->data.block_size) {
                const uint64_t cur_block_new_data_size =
                    std::min(data.size() + offset, (size_t)sb->data.block_size) - offset;
                std::vector<uint8_t> data_buffer(sb->data.block_size);
                iocontext->read_block(inode.block_lba, data_buffer);
                std::memcpy(data_buffer.data() + offset, data.data(), cur_block_new_data_size);
                iocontext->write_block(inode.block_lba, data_buffer);
                inode.size = std::max(inode.size, offset + cur_block_new_data_size);
                offset += cur_block_new_data_size;
                data = data.subspan(cur_block_new_data_size);
            }
            inode.block_lba = blkidxer->insert_block(0, 0, inode.block_lba);
            inode.storage_type = StorageType::Index;
        }
        if (inode.storage_type == StorageType::Index) {
            // 已有盘块写入
            uint64_t begin_idx = offset / sb->data.block_size;
            uint64_t end_idx =
                (std::min(offset + data.size(), inode.size) + sb->data.block_size - 1) /
                sb->data.block_size;

            std::vector<uint64_t> data_block_lbas =
                blkidxer->find_blocks(inode.block_lba, begin_idx, end_idx);
            offset %= sb->data.block_size;
            for (uint64_t i = 0; i < data_block_lbas.size(); i++) {
                std::vector<uint8_t> data_block_buffer(sb->data.block_size);
                uint64_t cur_epoch_size = std::min(sb->data.block_size - offset, data.size());
                if (data_block_lbas[i]) {
                    iocontext->read_block(data_block_lbas[i], data_block_buffer);
                    std::memcpy(data_block_buffer.data() + offset, data.data(), cur_epoch_size);
                    iocontext->write_block(data_block_lbas[i], data_block_buffer);
                } else {
                    uint64_t new_block_lba;
                    if (!blkalloc->allocate_block(new_block_lba))
                        return false;
                    std::memcpy(data_block_buffer.data(), data.data(), cur_epoch_size);
                    iocontext->write_block(new_block_lba, data_block_buffer);
                    inode.block_lba =
                        blkidxer->insert_block(inode.block_lba, i + begin_idx, new_block_lba);
                }
                inode.size = std::max(inode.size, (begin_idx + i) * sb->data.block_size + offset +
                                                      cur_epoch_size);
                data = data.subspan(cur_epoch_size);
                offset = 0;
            }
            // 新盘块插入
            while (!data.empty()) {
                std::vector<uint8_t> data_block_buffer(sb->data.block_size);
                uint64_t cur_epoch_size = std::min(sb->data.block_size - offset, data.size());
                uint64_t new_block_lba;
                if (!blkalloc->allocate_block(new_block_lba))
                    return false;
                std::memcpy(data_block_buffer.data(), data.data(), cur_epoch_size);
                iocontext->write_block(new_block_lba, data_block_buffer);
                inode.block_lba = blkidxer->insert_block(inode.block_lba, ++end_idx, new_block_lba);
                data = data.subspan(cur_epoch_size);
                inode.size += cur_epoch_size;
                offset = 0;
            }
        }
        write_inode_to_disk(inode_id, &inode);
        return true;
    }

    // TODO: 判断是否存在
    bool get_inode(uint64_t id, INode *node) {
        std::vector<uint8_t> buffer(sb->data.block_size);
        iocontext->read_block(id / sb->data.inodes_per_block + sb->data.inode_block_start_lba,
                              buffer);
        std::memcpy(node, buffer.data() + (id % sb->data.inodes_per_block) * sb->data.inode_size,
                    sb->data.inode_size);
        return true;
    }

    bool write_inode_to_disk(uint64_t inode_id, INode *node) {
        uint64_t block_lba = inode_id / sb->data.inodes_per_block + sb->data.inode_block_start_lba;
        std::vector<uint8_t> buffer(sb->data.block_size);
        iocontext->read_block(inode_id / sb->data.inodes_per_block + sb->data.inode_block_start_lba,
                              buffer);
        std::memcpy(buffer.data() + (inode_id % sb->data.inodes_per_block) * sb->data.inode_size,
                    node, sb->data.inode_size);
        iocontext->write_block(block_lba, buffer);
        return true;
    }

    uint64_t find_inode_by_name(uint64_t dir_inode_id, std::string name) {
        INode dir_inode;
        if (!get_inode(dir_inode_id, &dir_inode) || dir_inode.file_type != FileType::Directory)
            return 0;
        std::vector<uint8_t> buffer;
        uint64_t cur_size = 0;
        while (cur_size < dir_inode.size) {
            buffer = read_data(dir_inode_id, cur_size, sb->data.block_size);
            for (uint64_t i = 0; i < buffer.size(); i += sb->data.diritem_size) {
                DirItem *item = reinterpret_cast<DirItem *>(buffer.data() + i);
                if (item->name == name) {
                    return item->inode_id;
                }
            }
            cur_size += sb->data.block_size;
        }
        return 0;
    }

private:
    std::shared_ptr<SuperBlock> sb;
    std::shared_ptr<IOContext> iocontext;
    std::shared_ptr<BlockAllocator> blkalloc;
    std::shared_ptr<BlockIndexer> blkidxer;
};
