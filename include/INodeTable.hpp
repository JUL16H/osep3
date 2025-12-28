#pragma once
#include "BlockAllocator.hpp"
#include "BlockIndexer.hpp"
#include "INode.hpp"
#include "IOContext.hpp"

struct DirItem {
    uint64_t inode_id;
    char name[FILENAME_SIZE];
};
static_assert(sizeof(DirItem) == DIRITEM_SIZE);

class INodeTable {
    struct CacheItem {
        uint64_t id;
        INode node;
        bool dirty = false;
    };

public:
    INodeTable(std::shared_ptr<SuperBlock> _sb, std::shared_ptr<IOContext> _ioc,
               std::shared_ptr<BlockAllocator> _blkalloc, std::shared_ptr<BlockIndexer> _blkidxer,
               uint64_t _cache_size = 16384)
        : sb(_sb), iocontext(_ioc), blkalloc(_blkalloc), blkidxer(_blkidxer),
          max_cache_size(_cache_size) {}

    ~INodeTable() { flush(); }

    void reset_inode_bitmap() {
        spdlog::debug("[INodeManager] 写入INode位图");
        std::shared_ptr<Buffer> buffer;
        for (uint64_t i = 0; i < sb->data.inode_valid_blocks_cnt; i++) {
            buffer = iocontext->acquire_block(i + sb->data.inode_valid_block_start_lba);
            std::ranges::fill(*buffer, 0);
        }
        spdlog::debug("[INodeManager] INode位图写入完成");
    }

    std::optional<uint64_t> allocate_inode(FileType type) {
        spdlog::debug("[INodeManager] 查找空闲INode.");
        bool find = false;
        uint64_t bitmap_block_idx, byte_idx;
        uint8_t bit_idx;

        for (bitmap_block_idx = 0; bitmap_block_idx < sb->data.inode_valid_blocks_cnt;
             bitmap_block_idx++) {
            std::shared_ptr<const Buffer> cur_buffer =
                iocontext->acquire_block(bitmap_block_idx + sb->data.inode_valid_block_start_lba);
            for (byte_idx = 0; byte_idx < sb->data.block_size; byte_idx++) {
                if ((*cur_buffer)[byte_idx] != 0xff) {
                    find = true;
                    for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                        if (~(*cur_buffer)[byte_idx] & (1 << (7 - bit_idx)))
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
            return std::nullopt;
        }

        std::shared_ptr<Buffer> buffer =
            iocontext->acquire_block(bitmap_block_idx + sb->data.inode_valid_block_start_lba);
        (*buffer)[byte_idx] |= (1 << (7 - bit_idx));
        uint64_t id = bitmap_block_idx * sb->data.bits_per_block + byte_idx * 8 + bit_idx;
        spdlog::debug("[INodeManager] 找到空闲INode, id: {}", id);

        sb->data.free_inodes--;

        auto it = get(id);
        INode *node = &it->node;
        node->file_type = type;
        it->dirty = true;

        return id;
    }

    void free_inode(uint64_t id) {
        INode *node = &get(id)->node;

        uint64_t lba = id / sb->data.bits_per_block + sb->data.inode_valid_block_start_lba;
        uint64_t byte_idx = (id % sb->data.bits_per_block) / 8;
        uint64_t bit_idx = id % 8;

        if (node->storage_type == StorageType::Direct) {
            blkalloc->free_block(node->block_lba);
        } else if (node->storage_type == StorageType::Index) {
            blkidxer->free_node(node->block_lba);
        }

        std::memset(&get(id)->node, 0, sb->data.inode_size);
        get(id)->dirty = true;

        std::shared_ptr<Buffer> buffer = iocontext->acquire_block(lba);
        (*buffer)[byte_idx] &= ~((uint8_t)1 << (7 - bit_idx));

        sb->data.free_inodes++;
    }

    size_t read_data(uint64_t id, uint64_t offset, std::span<uint8_t> data) {
        spdlog::debug("[INodeTable] 读取数据, id: {}.", id);
        INode *node = &get(id)->node;

        const uint64_t size = std::min(data.size(), node->size - offset);

        if (node->storage_type == StorageType::Inline) {
            std::memcpy(data.data(), node->inline_data + offset, size);
        } else if (node->storage_type == StorageType::Direct) {
            std::shared_ptr<const Buffer> data_block_buffer =
                iocontext->read_block(node->block_lba);
            std::memcpy(data.data(), data_block_buffer->data() + offset, size);
        } else if (node->storage_type == StorageType::Index) {
            uint64_t in_block_offset = offset % sb->data.block_size;
            uint64_t write_pos = 0;
            uint64_t cur_epoch_size;
            uint64_t cur_lba;
            uint64_t remain_size = size;

            std::shared_ptr<const Buffer> data_block_buffer;
            for (uint64_t i = offset / sb->data.block_size;
                 i <= (offset + size - 1) / sb->data.block_size; i++) {
                cur_lba = blkidxer->find_block(node->block_lba, i).value_or(0);
                cur_epoch_size = std::min(remain_size, sb->data.block_size - in_block_offset);
                if (cur_lba != 0) {
                    data_block_buffer = iocontext->read_block(cur_lba);
                    std::memcpy(data.data() + write_pos,
                                data_block_buffer->data() + in_block_offset, cur_epoch_size);
                } else {
                    std::memset(data.data() + write_pos, 0, cur_epoch_size);
                }
                in_block_offset = 0;
                write_pos += cur_epoch_size;
                remain_size -= cur_epoch_size;
            }
        }
        return size;
    }

    bool write_data(uint64_t id, uint64_t offset, std::span<uint8_t> data) {
        spdlog::debug("[INodeTable] 写入数据, id: {}.", id);
        if (data.empty())
            return true;

        auto it = get(id);
        INode *node = &it->node;
        it->dirty = true;

        if (node->storage_type == StorageType::Inline) {
            if (offset + data.size() <= sb->data.inode_inline_data_size) {
                std::memcpy(node->inline_data + offset, data.data(), data.size());
                node->size = std::max(node->size, offset + data.size());
                write_inode_to_disk(id, node);
                return true;
            }
            // 数据超出 Inline 范围
            auto data_block_lba = blkalloc->allocate_block();
            if (!data_block_lba) {
                return false;
            }
            std::shared_ptr<Buffer> data_buffer = iocontext->acquire_block(data_block_lba.value());
            std::memcpy(data_buffer->data(), node->inline_data, node->size);
            std::memset(node->inline_data, 0, node->size);
            if (offset <= sb->data.block_size) {
                const uint64_t cur_block_new_data_size =
                    std::min(data.size() + offset, (size_t)sb->data.block_size) - offset;
                std::memcpy(data_buffer->data() + offset, data.data(), cur_block_new_data_size);
                node->size = std::max(node->size, offset + cur_block_new_data_size);
                offset += cur_block_new_data_size;
                data = data.subspan(cur_block_new_data_size);
            }
            node->block_lba = data_block_lba.value();
            node->storage_type = StorageType::Direct;
            if (data.empty()) {
                write_inode_to_disk(id, node);
                return true;
            }
        }
        if (node->storage_type == StorageType::Direct) {
            if (data.size() + offset <= sb->data.block_size) {
                std::shared_ptr<Buffer> data_buffer = iocontext->acquire_block(node->block_lba);
                std::memcpy(data_buffer->data() + offset, data.data(), data.size());
                node->size = std::max(node->size, offset + data.size());
                write_inode_to_disk(id, node);
                return true;
            }
            // 数据超出 Direct 范围
            if (offset <= sb->data.block_size) {
                const uint64_t cur_block_new_data_size =
                    std::min(data.size() + offset, (size_t)sb->data.block_size) - offset;
                std::shared_ptr<Buffer> data_buffer = iocontext->acquire_block(node->block_lba);
                std::memcpy(data_buffer->data() + offset, data.data(), cur_block_new_data_size);
                node->size = std::max(node->size, offset + cur_block_new_data_size);
                offset += cur_block_new_data_size;
                data = data.subspan(cur_block_new_data_size);
            }
            auto optlba = blkidxer->insert_block(0, 0, node->block_lba);
            if (!optlba)
                return false;
            node->block_lba = optlba.value();
            node->storage_type = StorageType::Index;
        }
        if (node->storage_type == StorageType::Index) {
            auto cur_pos = offset;
            const auto end_pos = offset + data.size();
            while (!data.empty()) {
                uint64_t cur_blk_idx = cur_pos / sb->data.block_size;
                uint64_t in_blk_offset = cur_pos % sb->data.block_size;
                uint64_t batch_size =
                    std::min(data.size(), (size_t)sb->data.block_size - in_blk_offset);

                uint64_t blk_lba = blkidxer->find_block(node->block_lba, cur_blk_idx).value_or(0);

                if (blk_lba == 0) {
                    auto new_blk_lba = blkalloc->allocate_block();
                    if (!new_blk_lba)
                        return false;
                    auto optlba =
                        blkidxer->insert_block(node->block_lba, cur_blk_idx, new_blk_lba.value());
                    if (!optlba)
                        return false;
                    node->block_lba = optlba.value();
                    blk_lba = new_blk_lba.value();
                }
                std::shared_ptr<Buffer> blk_buffer = iocontext->acquire_block(blk_lba);
                std::memcpy(blk_buffer->data() + in_blk_offset, data.data(), batch_size);

                data = data.subspan(batch_size);
                cur_pos += batch_size;
            }
            node->size = std::max(end_pos, node->size);
        }
        return true;
    }

    bool add_diritem(uint64_t id, std::string name, uint64_t to) {
        spdlog::debug("[INodeTable] 添加目录项, id: {}, name: {}, to id: {}.", id, name, to);

        INode *node = &get(id)->node;
        if (node->file_type != FileType::Directory)
            return false;

        std::vector<uint8_t> new_item_buffer(sb->data.diritem_size);
        DirItem *new_item = reinterpret_cast<DirItem *>(new_item_buffer.data());
        uint16_t name_size = std::min((size_t)sb->data.filename_size - 1, name.size());
        std::memcpy(new_item->name, name.data(), name_size);
        new_item->name[name_size] = '\0';
        new_item->inode_id = to;

        if (find_inode_by_name(id, new_item->name).has_value())
            return false;

        write_data(id, node->size, new_item_buffer);
        get(id)->dirty = true;
        if (id != to)
            get(to)->node.link_cnt++;
        return true;
    }

    // TODO: 检查id合法性
    bool remove_diritem(uint64_t id, std::string name) {
        if (name == "." || name == "..")
            return false;
        INode *node = &get(id)->node;
        std::vector<uint8_t> buffer(sb->data.diritem_size);
        for (uint64_t i = 0; i < node->size; i += sb->data.diritem_size) {
            read_data(id, i, buffer);
            DirItem *item = reinterpret_cast<DirItem *>(buffer.data());
            if (item->name != name)
                continue;
            INode *item_node = &get(item->inode_id)->node;
            if (item_node->file_type == FileType::Directory && !is_dir_empty(item->inode_id))
                return false;
            if (--item_node->link_cnt == 0) {
                free_inode(item->inode_id);
            }
            read_data(id, node->size - sb->data.diritem_size, buffer);
            write_data(id, i, buffer);
            node->size -= sb->data.diritem_size;
            get(id)->dirty = true;
            return true;
        }
        return false;
    }

    // TODO: 判断是否存在
    bool get_inode_from_disk(uint64_t id, INode *node) {
        std::shared_ptr<const Buffer> buffer =
            iocontext->read_block(id / sb->data.inodes_per_block + sb->data.inode_block_start_lba);
        std::memcpy(node, buffer->data() + (id % sb->data.inodes_per_block) * sb->data.inode_size,
                    sb->data.inode_size);
        return true;
    }

    bool write_inode_to_disk(uint64_t inode_id, INode *node) {
        uint64_t block_lba = inode_id / sb->data.inodes_per_block + sb->data.inode_block_start_lba;
        std::shared_ptr<Buffer> buffer = iocontext->acquire_block(
            inode_id / sb->data.inodes_per_block + sb->data.inode_block_start_lba);
        std::memcpy(buffer->data() + (inode_id % sb->data.inodes_per_block) * sb->data.inode_size,
                    node, sb->data.inode_size);
        return true;
    }

    std::optional<uint64_t> find_inode_by_name(uint64_t dir_inode_id, std::string name) {
        INode *dir_inode = &get(dir_inode_id)->node;
        const uint64_t epoch_num = 1024;
        for (uint64_t cur_offset = 0; cur_offset < dir_inode->size;
             cur_offset += epoch_num * sb->data.diritem_size) {
            uint64_t epoch_size =
                std::min(epoch_num * sb->data.diritem_size, dir_inode->size - cur_offset);
            std::vector<uint8_t> buffer(epoch_size);
            auto size = read_data(dir_inode_id, cur_offset, buffer);
            buffer.resize(size);
            for (uint64_t i = 0; i < buffer.size(); i += sb->data.diritem_size) {
                auto *item = reinterpret_cast<DirItem *>(buffer.data() + i);
                if (std::string(item->name) == name)
                    return item->inode_id;
            }
        }
        return std::nullopt;
    }

    INode get_inode_info(uint64_t id) { return get(id)->node; }

    void flush() {
        for (auto it : cache_list)
            if (it.dirty)
                write_inode_to_disk(it.id, &it.node);
        clear_cache();
    }

    void clear_cache() {
        cache_list.clear();
        cache_mp.clear();
    }

    bool is_dir_empty(uint64_t id) {
        auto it = get(id);
        return it->node.size == 2 * sb->data.diritem_size;
    }

private:
    std::list<CacheItem>::iterator get(uint64_t id) {
        if (cache_mp.count(id)) {
            this->cache_list.splice(cache_list.begin(), cache_list, cache_mp[id]);
            return cache_mp[id];
        }
        if (cache_list.size() < max_cache_size) {
            INode node;
            get_inode_from_disk(id, &node);
            cache_list.push_front(CacheItem{.id = id, .node = node});
            cache_mp[id] = cache_list.begin();
            return cache_mp[id];
        }

        CacheItem old_item = cache_list.back();
        if (old_item.dirty)
            write_inode_to_disk(old_item.id, &old_item.node);
        cache_list.pop_back();
        cache_mp.erase(old_item.id);

        INode node;
        get_inode_from_disk(id, &node);
        cache_list.push_front(CacheItem{.id = id, .node = node});
        cache_mp[id] = cache_list.begin();
        return cache_mp[id];
    }

private:
    std::shared_ptr<SuperBlock> sb;
    std::shared_ptr<IOContext> iocontext;
    std::shared_ptr<BlockAllocator> blkalloc;
    std::shared_ptr<BlockIndexer> blkidxer;

    const uint32_t max_cache_size;
    std::list<CacheItem> cache_list;
    std::unordered_map<uint64_t, typename decltype(cache_list)::iterator> cache_mp;
};

// TODO:
class INodeDataIterator {};
