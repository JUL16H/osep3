#pragma once
#include "BitmapManager.hpp"
#include "BlockManager.hpp"
#include "IDisk.hpp"
#include "INode.hpp"
#include "INodeManager.hpp"
#include "SuperBlock.hpp"
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
    FileSys(IDisk *_disk) : disk(_disk) {
        spdlog::info("[FileSys] 文件系统启动.");
        BlockManager *block_manager = BlockManager::get_instance();
        BitmapManager *bitmap_manager = BitmapManager::get_instance();
        INodeManager *inode_manager = INodeManager::get_instance();

        super_block = std::make_shared<SuperBlock>();
        block_manager->set_disk(disk);

        block_manager->set_super_block(super_block);
        inode_manager->set_super_block(super_block);
        bitmap_manager->set_super_block(super_block);
        data_btree->set_super_block(super_block);

        spdlog::info("[FileSys] 读取Super Block.");
        block_manager->read_super_block();

        if (!super_block->valid()) {
            spdlog::info("[FileSys] 文件系统不匹配, 执行硬盘格式化.");
            format();
            spdlog::info("[FileSys] 重新读取Super Block.");
            block_manager->read_super_block();
        }

        debug_super_block_info();
    }

    void debug_super_block_info() {
        spdlog::debug("[FileSys] 硬盘Super Block信息:");
        spdlog::debug("[FileSys] Magic Number: 0x{:X}.", super_block->data.magic_number);
        spdlog::debug("[FileSys] Version: {}.", super_block->data.version);
        spdlog::debug("[FileSys] Disk Size: {} GB.", super_block->data.disk_size_gb);
        spdlog::debug("[FileSys] Block Size: {} B.", super_block->data.block_size);
        spdlog::debug("[FileSys] Total Blocks: {}.", super_block->data.total_blocks);
        spdlog::debug("[FileSys] Super Block Start LBA: 0x{:X}.",
                      super_block->data.super_block_start_lba);
        spdlog::debug("[FileSys] Super Blocks Count: {}.", super_block->data.super_blocks_cnt);
        spdlog::debug("[FileSys] Bitmap Block Start LBA: 0x{:x}.",
                      super_block->data.bitmap_block_start_lba);
        spdlog::debug("[FileSys] Bitmap Blocks Count: {}.", super_block->data.bitmap_blocks_cnt);
        spdlog::debug("[FileSys] INode Size: {} B.", super_block->data.inode_size);
        spdlog::debug("[FileSys] INodes Count: {}.", super_block->data.inode_size);
        spdlog::debug("[FileSys] INode Valid Block Start LBA: 0x{:X}.",
                      super_block->data.inode_valid_block_start_lba);
        spdlog::debug("[FileSys] INode Valid Blocks Count: {}.",
                      super_block->data.inode_valid_blocks_cnt);
        spdlog::debug("[FileSys] INode Block Start LBA: 0x{:X}.",
                      super_block->data.inode_block_start_lba);
        spdlog::debug("[FileSys] INode Blocks Count: {}.", super_block->data.inode_blocks_cnt);
        spdlog::debug("[FileSys] Basic Blocks Count: {}.", super_block->data.basic_blocks_cnt);
        spdlog::debug("[FileSys] Root INode: 0x{:X}.", super_block->data.root_inode);
        spdlog::debug("[FileSys] Free Blocks: {}.", super_block->data.free_blocks);
    }

    void format() {
        spdlog::info("[FileSys] 进行硬盘格式化.");

        spdlog::debug("[FileSys] 清空硬盘.");
        block_manager->clear();

        spdlog::debug("[FileSys] 写入Super Block.");
        *super_block = create_superblock(disk->get_disk_size());
        block_manager->refresh_super_block();

        spdlog::debug("[FileSys] 写入bitmap.");
        bitmap_manager->reset_bitmap();

        spdlog::debug("[FileSys] 写入INode bitmap.");
        inode_manager->reset_inode_bitmap();

        spdlog::debug("[FileSys] 创建根目录.");
        create_dir(super_block->data.root_inode, "/");

        spdlog::info("[FileSys] 格式化完成.");
    }

private:
    // TODO: path_inode_id -> path_str
    void create_dir(uint32_t path_inode_id, std::string name) {
        spdlog::info("[FileSys] 创建文件夹.");
        spdlog::debug("[FileSys] 目录名: {}, 父目录INode: {}.", name, path_inode_id);

        uint64_t inode_id;
        if (!inode_manager->allocate_inode(inode_id)) {
            spdlog::warn("[FileSys] 无空闲INode.");
            return;
        }

        spdlog::debug("[FileSys] 填充INode信息.");
        INode inode(inode_id, path_inode_id);
        inode.file_type = FileType::Directory;
        inode.storage_type = StorageType::Inline;
        inode.block_lba = 0; // 不使用
        inode.size = 2 * super_block->data.diritem_size;

        spdlog::debug("[FileSys] 目录写入INode.");
        DirItem basic_dir[2] = {DirItem(inode_id, "."), DirItem(path_inode_id, "..")};
        std::memcpy(inode.inline_data, basic_dir, 2 * DIRITEM_SIZE);

        spdlog::debug("[FileSys] INode写入硬盘.");
        std::vector<uint8_t> buffer(super_block->data.block_size);
        uint32_t inode_block_lba =
            super_block->data.inode_block_start_lba + inode_id / super_block->data.inodes_per_block;
        block_manager->read_block(inode_block_lba, buffer);
        std::memcpy(buffer.data() + (inode_id % super_block->data.inodes_per_block) *
                                        super_block->data.inode_size,
                    &inode, sizeof(inode));
        block_manager->write_block(inode_block_lba, buffer);

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
        inode_manager->get_inode_info(path_inode_id, inode);
        spdlog::debug("[FileSys] INode Size: {} B", inode.size);

        std::vector<uint8_t> items_buffer(super_block->data.block_size);
        uint64_t processed_size = 0;
        uint64_t block_idx = 0;

        while (processed_size < inode.size) {
            if (!inode_manager->get_inode_data_block(path_inode_id, block_idx, items_buffer)) {
                spdlog::error("[FileSys] 读取目录数据块失败 idx: {}", block_idx);
                break;
            }

            uint64_t current_block_data_size =
                std::min((uint64_t)super_block->data.block_size, inode.size - processed_size);

            for (uint32_t j = 0; j < current_block_data_size; j += sizeof(DirItem)) {
                DirItem *item = reinterpret_cast<DirItem *>(items_buffer.data() + j);
                if (item->valid) {
                    std::cout << std::format("{:9d} {}\n", item->inode_id, item->name);
                }
            }

            processed_size += current_block_data_size;
            block_idx++;
        }
    }

    void dir_add_item(uint64_t dir_inode_id, uint64_t item_inode_id, std::string item_name) {
        spdlog::info("[FileSys] 目录添加项.");
        // TODO:
        // if (名字有了) return false; 函数签名void -> bool

        spdlog::debug("[FileSys] 填充目录项信息.");
        DirItem new_item(item_inode_id, item_name);

        inode_manager->inode_add_data(dir_inode_id,
                                      std::span<uint8_t>(reinterpret_cast<uint8_t *>(&new_item),
                                                         super_block->data.diritem_size));
    }

private:
    std::shared_ptr<IDisk> disk;
    std::shared_ptr<SuperBlock> super_block;
    BlockManager *block_manager = BlockManager::get_instance();
    BitmapManager *bitmap_manager = BitmapManager::get_instance();
    INodeManager *inode_manager = INodeManager::get_instance();
    DataBTree *data_btree = DataBTree::get_instance();
};
