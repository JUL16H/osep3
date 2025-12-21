#pragma oncefile
#include "BitmapManager.hpp"
#include "BlockManager.hpp"
#include "INode.hpp"
#include "INodeManager.hpp"
#include "SuperBlock.hpp"
#include "VDisk.hpp"
#include "macros.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>

// TODO:
// 参数设定合理性检查
// uintN_t 溢出冗余检查

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

class FileSys {
    friend int main(); // HACK: Just added when DEBUG.
public:
    FileSys(IDisk &_disk)
        : disk(_disk), block_manager(disk, super_block), bitmap_manager(super_block, block_manager),
          inode_manager(super_block, block_manager, bitmap_manager) {
        spdlog::info("[FileSys] 文件系统启动.");

        spdlog::info("[FileSys] 读取Super Block.");
        block_manager.read_super_block();

        if (!super_block.valid()) {
            spdlog::info("[FileSys] 文件系统不匹配, 执行硬盘格式化.");
            format();
            spdlog::info("[FileSys] 重新读取Super Block.");
            block_manager.read_super_block();
        }

        debug_super_block_info();
    }

    void debug_super_block_info() {
        spdlog::debug("[FileSys] 硬盘Super Block信息:");
        spdlog::debug("[FileSys] Magic Number: 0x{:X}.", super_block.data.magic_number);
        spdlog::debug("[FileSys] Version: {}.", super_block.data.version);
        spdlog::debug("[FileSys] Disk Size: {} GB.", super_block.data.disk_size_gb);
        spdlog::debug("[FileSys] Block Size: {} B.", super_block.data.block_size);
        spdlog::debug("[FileSys] Total Blocks: {}.", super_block.data.total_blocks);
        spdlog::debug("[FileSys] Super Block Start LBA: 0x{:X}.",
                      super_block.data.super_block_start_lba);
        spdlog::debug("[FileSys] Super Blocks Count: {}.", super_block.data.super_blocks_cnt);
        spdlog::debug("[FileSys] Bitmap Block Start LBA: 0x{:x}.",
                      super_block.data.bitmap_block_start_lba);
        spdlog::debug("[FileSys] Bitmap Blocks Count: {}.", super_block.data.bitmap_blocks_cnt);
        spdlog::debug("[FileSys] INode Size: {} B.", super_block.data.inode_size);
        spdlog::debug("[FileSys] INodes Count: {}.", super_block.data.inode_size);
        spdlog::debug("[FileSys] INode Valid Block Start LBA: 0x{:X}.",
                      super_block.data.inode_valid_block_start_lba);
        spdlog::debug("[FileSys] INode Valid Blocks Count: {}.",
                      super_block.data.inode_valid_blocks_cnt);
        spdlog::debug("[FileSys] INode Block Start LBA: 0x{:X}.",
                      super_block.data.inode_block_start_lba);
        spdlog::debug("[FileSys] INode Blocks Count: {}.", super_block.data.inode_blocks_cnt);
        spdlog::debug("[FileSys] Basic Blocks Count: {}.", super_block.data.basic_blocks_cnt);
        spdlog::debug("[FileSys] Root INode: 0x{:X}.", super_block.data.root_inode);
        spdlog::debug("[FileSys] Free Blocks: {}.", super_block.data.free_blocks);
    }

    void format() {
        spdlog::info("[FileSys] 进行硬盘格式化.");

        spdlog::debug("[FileSys] 清空硬盘.");
        block_manager.clear();

        spdlog::debug("[FileSys] 写入Super Block.");
        super_block = create_superblock(disk.get_disk_size());
        block_manager.refresh_super_block();

        spdlog::debug("[FileSys] 写入bitmap.");
        bitmap_manager.reset_bitmap();

        spdlog::debug("[FileSys] 写入INode bitmap.");
        inode_manager.reset_inode_bitmap();

        spdlog::debug("[FileSys] 创建根目录.");
        create_dir(super_block.data.root_inode, "/");

        spdlog::info("[FileSys] 格式化完成.");
    }

private:
    void create_dir(uint32_t path_inode_id, std::string name) {
        spdlog::info("[FileSys] 创建文件夹.");
        spdlog::debug("[FileSys] 目录名: {}, 父目录INode: {}.", name, path_inode_id);

        uint64_t inode_id;
        if (!inode_manager.allocate_inode(inode_id)) {
            spdlog::warn("[FileSys] 无空闲INode.");
            return;
        }

        spdlog::debug("[FileSys] 填充INode信息.");
        INode inode(inode_id, path_inode_id);
        inode.file_type = FileType::Directory;
        inode.storage_type = StorageType::Inline;
        inode.block_lba = 0; // 不使用
        inode.size = 2 * super_block.data.diritem_size;

        spdlog::debug("[FileSys] 目录写入INode.");
        DirItem basic_dir[2] = {DirItem(inode_id, "."), DirItem(path_inode_id, "..")};
        std::memcpy(inode.inline_data, basic_dir, 2 * DIRITEM_SIZE);

        spdlog::debug("[FileSys] INode写入硬盘.");
        std::vector<uint8_t> buffer(super_block.data.block_size);
        uint32_t inode_block_lba =
            super_block.data.inode_block_start_lba + inode_id / super_block.data.inodes_per_block;
        block_manager.read_block(inode_block_lba, buffer);
        std::memcpy(buffer.data() + (inode_id % super_block.data.inodes_per_block) *
                                        super_block.data.inode_size,
                    &inode, sizeof(inode));
        block_manager.write_block(inode_block_lba, buffer);

        if (name == "/")
            return;

        dir_add_item(path_inode_id, inode_id, name);
    }

    ~FileSys() {
        // block_manager.clear();
    }

    // FIX:现在size不能超过BLOCK_SIZE

    void list_directory(uint64_t path_inode_id) {
        spdlog::info("[FileSys] 陈列目录项.");

        INode inode;
        read_inode(path_inode_id, &inode);
        spdlog::debug("[FileSys] INode Size: {} B", inode.size);

        uint64_t size;
        std::vector<DirItem> items(super_block.data.block_size / super_block.data.diritem_size);
        for (uint64_t i = 0; i * BLOCK_SIZE <= inode.size; i++) {
            inode_read_data(inode, i,
                            std::span<uint8_t, BLOCK_SIZE>(
                                reinterpret_cast<uint8_t *>(items.data()), BLOCK_SIZE),
                            size);
            for (uint32_t j = 0; j < size / DIRITEM_SIZE; j++) {
                std::cout << std::format("{:9d} {}\n", items[j].inode_id, items[j].name);
            }
        }
    }

    // TODO:
    bool dir_add_item(uint64_t dir_inode_id, uint64_t item_inode_id, std::string item_name) {
        spdlog::info("[FileSys] 目录添加项.");

        spdlog::debug("[FileSys] 填充目录项信息.");
        DirItem new_item(item_inode_id, item_name);

        inode_manager.inode_add(dir_inode_id,
                                std::span<uint8_t>(reinterpret_cast<uint8_t *>(&new_item),
                                                   super_block.data.diritem_size));

        return false;
    }

    // 好像没什么用.
    bool is_empty_block(uint32_t lba) {
        std::vector<uint8_t> buffer(super_block.data.block_size);
        uint32_t bitmap_lba =
            lba / super_block.data.bits_per_block + super_block.data.bitmap_block_start_lba;
        uint32_t byte_idx = (lba % super_block.data.bits_per_block) / 8;
        uint8_t bit_idx = lba % 8;
        block_manager.read_block(bitmap_lba, buffer);

        bool is_occupied = buffer[byte_idx] & (1 << (7 - bit_idx));
        return !is_occupied;
    }

    void read_inode(uint64_t inode_id, INode *p) {
        std::vector<uint8_t> buffer(super_block.data.block_size);
        uint64_t lba =
            inode_id / super_block.data.inodes_per_block + super_block.data.inode_block_start_lba;
        uint64_t offset =
            inode_id % super_block.data.inodes_per_block * super_block.data.inode_size;
        block_manager.read_block(lba, buffer);
        std::memcpy(p, buffer.data() + offset, super_block.data.inode_size);
    }

    // TODO: 封装进INodeManager
    bool inode_read_data(const INode &inode, uint64_t block_idx,
                         std::span<uint8_t, BLOCK_SIZE> buffer, uint64_t &size) {
        spdlog::debug("[FileSys] 读取INode信息, INode ID: 0x{:X}, 盘块号: {}.", inode.ID,
                      block_idx);
        if (inode.size < block_idx * super_block.data.block_size) {
            spdlog::warn("[FileSys] 盘块超出INode持有范围.");
            return false;
        }

        switch (inode.storage_type) {
        case StorageType::Inline:
            size = inode.size;
            std::memcpy(buffer.data(), inode.inline_data, size);
            break;
        case StorageType::Direct:
            size = inode.size;
            block_manager.read_block(inode.block_lba, buffer);
            break;
        // TODO:
        case StorageType::BTree:
            break;
        }

        spdlog::debug("[FileSys] 读取完成.");
        return true;
    }

private:
    IDisk &disk;
    SuperBlock super_block;
    BlockManager block_manager;
    BitmapManager bitmap_manager;
    INodeManager inode_manager;
};
