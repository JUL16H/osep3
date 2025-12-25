#pragma once
#include "BlockAllocator.hpp"
#include "IDisk.hpp"
#include "INode.hpp"
#include "INodeTable.hpp"
#include "IOContext.hpp"
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


class FileSys {
    friend int main(); // HACK: Just added when DEBUG.
public:
    FileSys(IDisk *_disk) : disk(_disk) {
        spdlog::info("[FileSys] 文件系统启动.");
        sb = std::make_shared<SuperBlock>();
        iocontext = std::make_shared<IOContext>(sb, disk);
        blkalloc = std::make_shared<BlockAllocator>(sb, iocontext);
        blkidxer = std::make_shared<BlockIndexer>(sb, iocontext, blkalloc);
        inodetable = std::make_shared<INodeTable>(sb, iocontext, blkalloc, blkidxer);

        spdlog::info("[FileSys] 读取Super Block.");
        iocontext->read_super_block();

        if (!sb->valid()) {
            spdlog::info("[FileSys] 文件系统不匹配, 执行硬盘格式化.");
            format();
            spdlog::info("[FileSys] 重新读取Super Block.");
            iocontext->read_super_block();
        }

        debug_super_block_info();
    }

    void debug_super_block_info() {
        spdlog::debug("[FileSys] 硬盘Super Block信息:");
        spdlog::debug("[FileSys] Magic Number: 0x{:X}.", sb->data.magic_number);
        spdlog::debug("[FileSys] Version: {}.", sb->data.version);
        spdlog::debug("[FileSys] Disk Size: {} GB.", sb->data.disk_size_gb);
        spdlog::debug("[FileSys] Block Size: {} B.", sb->data.block_size);
        spdlog::debug("[FileSys] Total Blocks: {}.", sb->data.total_blocks);
        spdlog::debug("[FileSys] Super Block Start LBA: 0x{:X}.", sb->data.super_block_start_lba);
        spdlog::debug("[FileSys] Super Blocks Count: {}.", sb->data.super_blocks_cnt);
        spdlog::debug("[FileSys] Bitmap Block Start LBA: 0x{:x}.", sb->data.bitmap_block_start_lba);
        spdlog::debug("[FileSys] Bitmap Blocks Count: {}.", sb->data.bitmap_blocks_cnt);
        spdlog::debug("[FileSys] INode Size: {} B.", sb->data.inode_size);
        spdlog::debug("[FileSys] INodes Count: {}.", sb->data.inode_size);
        spdlog::debug("[FileSys] INode Valid Block Start LBA: 0x{:X}.",
                      sb->data.inode_valid_block_start_lba);
        spdlog::debug("[FileSys] INode Valid Blocks Count: {}.", sb->data.inode_valid_blocks_cnt);
        spdlog::debug("[FileSys] INode Block Start LBA: 0x{:X}.", sb->data.inode_block_start_lba);
        spdlog::debug("[FileSys] INode Blocks Count: {}.", sb->data.inode_blocks_cnt);
        spdlog::debug("[FileSys] Basic Blocks Count: {}.", sb->data.basic_blocks_cnt);
        spdlog::debug("[FileSys] Root INode: 0x{:X}.", sb->data.root_inode);
        spdlog::debug("[FileSys] Free Blocks: {}.", sb->data.free_blocks);
    }

    void format() {
        spdlog::info("[FileSys] 进行硬盘格式化.");

        spdlog::debug("[FileSys] 清空硬盘.");
        iocontext->clear();

        spdlog::debug("[FileSys] 写入Super Block.");
        *sb = create_superblock(disk->get_disk_size());
        iocontext->refresh_super_block();

        spdlog::debug("[FileSys] 写入bitmap.");
        blkalloc->reset_bitmap();

        spdlog::debug("[FileSys] 写入INode bitmap.");
        inodetable->reset_inode_bitmap();

        spdlog::debug("[FileSys] 创建根目录.");
        create_dir(sb->data.root_inode, "/");

        spdlog::info("[FileSys] 格式化完成.");
    }

private:
    // TODO: path_inode_id -> path_str
    void create_dir(uint32_t path_inode_id, std::string name) {
        spdlog::info("[FileSys] 创建文件夹.");
        spdlog::debug("[FileSys] 目录名: {}, 父目录INode: {}.", name, path_inode_id);

        uint64_t inode_id;
        if (!inodetable->allocate_inode(inode_id)) {
            spdlog::warn("[FileSys] 无空闲INode.");
            return;
        }

        spdlog::debug("[FileSys] 填充INode信息.");
        INode inode(inode_id, path_inode_id);
        inode.file_type = FileType::Directory;
        inode.storage_type = StorageType::Inline;
        inode.block_lba = 0; // 不使用
        inode.size = 2 * sb->data.diritem_size;

        spdlog::debug("[FileSys] 目录写入INode.");
        DirItem basic_dir[2] = {DirItem(inode_id, "."), DirItem(path_inode_id, "..")};
        std::memcpy(inode.inline_data, basic_dir, 2 * DIRITEM_SIZE);

        spdlog::debug("[FileSys] INode写入硬盘.");
        std::vector<uint8_t> buffer(sb->data.block_size);
        uint32_t inode_block_lba =
            sb->data.inode_block_start_lba + inode_id / sb->data.inodes_per_block;
        iocontext->read_block(inode_block_lba, buffer);
        std::memcpy(buffer.data() + (inode_id % sb->data.inodes_per_block) * sb->data.inode_size,
                    &inode, sizeof(inode));
        iocontext->write_block(inode_block_lba, buffer);

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
        inodetable->get_inode(path_inode_id, &inode);
        spdlog::debug("[FileSys] INode Size: {} B", inode.size);

        std::vector<uint8_t> items_buffer(sb->data.block_size);
        uint64_t cur_size = 0;
        uint64_t block_idx = 0;

        while (cur_size < inode.size) {
            items_buffer = inodetable->read_data(path_inode_id, cur_size, sb->data.block_size);
            uint64_t current_block_data_size =
                std::min((uint64_t)sb->data.block_size, inode.size - cur_size);

            for (uint32_t j = 0; j < current_block_data_size; j += sizeof(DirItem)) {
                DirItem *item = reinterpret_cast<DirItem *>(items_buffer.data() + j);
                if (item->valid) {
                    std::cout << std::format("{:9d} {}\n", item->inode_id, item->name);
                }
            }

            cur_size += current_block_data_size;
            block_idx++;
        }
    }

    void dir_add_item(uint64_t dir_inode_id, uint64_t item_inode_id, std::string item_name) {
        spdlog::info("[FileSys] 目录添加项.");

        INode node;
        inodetable->get_inode(dir_inode_id, &node);

        // TODO:
        if (inodetable->find_inode_by_name(dir_inode_id, item_name))
            return;

        spdlog::debug("[FileSys] 填充目录项信息.");
        DirItem new_item(item_inode_id, item_name);

        inodetable->write_data(
            dir_inode_id, node.size,
            std::span<uint8_t>(reinterpret_cast<uint8_t *>(&new_item), sb->data.diritem_size));
    }

private:
    std::shared_ptr<IDisk> disk;
    std::shared_ptr<SuperBlock> sb;
    std::shared_ptr<IOContext> iocontext;
    std::shared_ptr<BlockAllocator> blkalloc;
    std::shared_ptr<INodeTable> inodetable;
    std::shared_ptr<BlockIndexer> blkidxer;
};
