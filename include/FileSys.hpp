#pragma once
#include "VDisk.hpp"
#include <cstdint>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

constexpr uint32_t BLOCK_SIZE = 16 * 1024;

constexpr uint64_t MAGIC_NUMBER = 0xEA6191;
constexpr uint64_t VERSION = 7;

constexpr uint32_t INODE_SIZE = 256;
constexpr uint32_t INODE_BLOCKS_COUNT = 4 * 1024;

constexpr uint16_t DIRITEM_SIZE = 64;

union SuperBlock {
    struct {
        uint64_t magic_number;
        uint64_t version;

        uint16_t disk_size_gb;
        uint32_t block_size;
        uint32_t total_blocks;
        uint32_t bits_per_block;

        uint32_t super_block_start_lba;
        uint16_t super_blocks_cnt;

        uint32_t bitmap_block_start_lba;
        uint16_t bitmap_blocks_cnt;

        uint32_t inode_size;
        uint32_t inodes_per_block;
        uint64_t inodes_cnt;
        uint64_t free_inodes;
        uint32_t inode_valid_block_start_lba;
        uint32_t inode_valid_blocks_cnt;
        uint32_t inode_block_start_lba;
        uint32_t inode_blocks_cnt;

        uint32_t basic_blocks_cnt;

        uint32_t root_inode;
        uint32_t free_blocks;
    } data;
    char padding[BLOCK_SIZE];
};
static_assert(sizeof(SuperBlock) == BLOCK_SIZE);

inline const SuperBlock create_superblock(uint32_t disk_size_gb) {
    SuperBlock rst;
    std::memset(&rst, 0, sizeof(SuperBlock));

    rst.data.magic_number = MAGIC_NUMBER;
    rst.data.version = VERSION;

    rst.data.disk_size_gb = disk_size_gb;
    rst.data.block_size = BLOCK_SIZE;
    rst.data.total_blocks = ((uint64_t)disk_size_gb << 30) / rst.data.block_size;
    rst.data.bits_per_block = rst.data.block_size * 8;

    rst.data.super_block_start_lba = 0;
    rst.data.super_blocks_cnt = 1;

    rst.data.bitmap_block_start_lba = rst.data.super_blocks_cnt;
    rst.data.bitmap_blocks_cnt = rst.data.total_blocks / rst.data.bits_per_block;

    rst.data.inode_size = INODE_SIZE;
    rst.data.inodes_per_block = rst.data.block_size / rst.data.inode_size;
    rst.data.inodes_cnt = rst.data.inodes_per_block * INODE_BLOCKS_COUNT;
    rst.data.free_inodes = rst.data.inodes_cnt;
    rst.data.inode_valid_block_start_lba =
        rst.data.bitmap_block_start_lba + rst.data.bitmap_blocks_cnt;
    rst.data.inode_valid_blocks_cnt =
        (rst.data.inodes_cnt + rst.data.bits_per_block - 1) / rst.data.bits_per_block;
    rst.data.inode_block_start_lba =
        rst.data.inode_valid_block_start_lba + rst.data.inode_valid_blocks_cnt;
    rst.data.inode_blocks_cnt = INODE_BLOCKS_COUNT;

    rst.data.basic_blocks_cnt = rst.data.super_blocks_cnt + rst.data.bitmap_blocks_cnt +
                                rst.data.inode_valid_blocks_cnt + rst.data.inode_blocks_cnt;

    rst.data.root_inode = 0;
    rst.data.free_blocks = rst.data.total_blocks - rst.data.basic_blocks_cnt;

    return rst;
}

union INode {
    struct {
        uint64_t ID;
        uint64_t prev_inode;
        uint32_t block_lba;
        uint32_t link_cnt;
        uint8_t type;
        uint64_t size;
    } data;
    char padding[INODE_SIZE];
};
static_assert(sizeof(INode) == INODE_SIZE);

struct DirItem {
    uint64_t inode;
    uint8_t valid;
    char name[54];
};
static_assert(sizeof(DirItem) == DIRITEM_SIZE);

inline INode create_INode(uint32_t _ID) {
    INode rst{.data{
        .ID = _ID,
        .link_cnt = 0,
        .size = 0,
    }};
    std::memset(&rst, 0, sizeof(rst));
    return rst;
}

class BPNodeHeader {
    uint8_t is_leaf;
};

class FileSys {
    friend int main();

public:
    FileSys(VDisk &_disk) : disk(_disk) {
        spdlog::info("[FileSys] 文件系统启动.");

        spdlog::info("[FileSys] 读取Super Block.");
        disk.read_block(0, BLOCK_SIZE, reinterpret_cast<char *>(&super_block));

        if (super_block.data.magic_number != MAGIC_NUMBER || super_block.data.version != VERSION ||
            super_block.data.block_size != BLOCK_SIZE ||
            super_block.data.inode_size != INODE_SIZE ||
            super_block.data.inode_blocks_cnt != INODE_BLOCKS_COUNT) {
            spdlog::info("[FileSys] 文件系统不匹配, 执行虚拟硬盘格式化.");
            format(disk.get_disk_size());
            spdlog::info("[FileSys] 重新读取Super Block.");
            read_block(0, &super_block);
        }

        spdlog::debug("[FileSys] 虚拟硬盘Super Block信息:");
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

        cur_inode = super_block.data.root_inode;
    }

    void format(uint32_t disk_size) {
        spdlog::info("[FileSys] 进行虚拟硬盘格式化.");
        spdlog::debug("[FileSys] 清空虚拟硬盘.");
        disk.clear();

        spdlog::debug("[FileSys] 写入Super Block.");
        super_block = create_superblock(disk.get_disk_size());
        write_block(0, &super_block);

        spdlog::debug("[FileSys] 写入位图.");
        char buffer[BLOCK_SIZE];
        std::ranges::fill(buffer, 0);
        // FIX:过多basic_block超过第一块bitmap
        for (uint64_t i = 0; i < super_block.data.basic_blocks_cnt / 8; i++)
            buffer[i] = 0xff;
        uint32_t remaining_bits = super_block.data.basic_blocks_cnt % 8;
        if (remaining_bits > 0)
            buffer[super_block.data.basic_blocks_cnt / 8] |= (0xff << (8 - remaining_bits));
        write_block(super_block.data.super_blocks_cnt, buffer);
        std::ranges::fill(buffer, 0);

        for (uint32_t i = 1; i < super_block.data.bitmap_blocks_cnt; i++)
            write_block(i + super_block.data.bitmap_block_start_lba, buffer);

        spdlog::debug("[FileSys] 写入INode位图.");
        for (uint32_t i = 1; i < super_block.data.inode_valid_blocks_cnt; i++)
            write_block(i + super_block.data.inode_valid_blocks_cnt, buffer);
        buffer[0] = 0x80;
        write_block(super_block.data.inode_valid_block_start_lba, buffer);

        spdlog::debug("[FileSys] 创建根目录.");
        create_dir(super_block.data.root_inode, "/");

        spdlog::info("[FileSys] 格式化完成.");
    }

    // FIX:
    void create_dir(uint32_t path_inode_id, std::string name) {
        spdlog::info("[FileSys] 创建文件夹.");
        spdlog::debug("[FileSys] 目录名: {}, 父目录INode: {}.", name, path_inode_id);

        uint64_t inode_id;
        if (!allocate_inode(inode_id))
            return;

        uint32_t block_lba;
        if (!allocate_block(block_lba))
            return;

        uint32_t inode_block_lba =
            super_block.data.inode_block_start_lba + inode_id / super_block.data.inodes_per_block;

        char buffer[BLOCK_SIZE];

        spdlog::debug("[FileSys] INode写入硬盘.");
        INode inode = create_INode(inode_id);
        inode.data.block_lba = block_lba;
        inode.data.prev_inode = path_inode_id;
        inode.data.size = 2;

        read_block(inode_block_lba, buffer);
        std::memcpy(buffer + (inode_id % super_block.data.inodes_per_block) *
                                 super_block.data.inode_size,
                    &inode, sizeof(inode));
        write_block(inode_block_lba, buffer);

        spdlog::debug("[FileSys] 目录写入硬盘.");
        std::memset(buffer, 0, BLOCK_SIZE);
        DirItem basic_dir[2] = {{
                                    .inode = inode_id,
                                    .valid = 1,
                                    .name = ".",
                                },
                                {
                                    .inode = path_inode_id,
                                    .valid = 1,
                                    .name = "..",
                                }};
        std::memcpy(buffer, basic_dir, 2 * DIRITEM_SIZE);
        write_block(block_lba, buffer);

        if (name == "/")
            return;

        // TODO: 父目录文件添加项
    }

    ~FileSys() {
        // disk.clear();
    }

private:
    void read_block(uint64_t lba, void *buffer) {
        disk.read_block(lba, super_block.data.block_size, reinterpret_cast<char *>(buffer));
    }

    void write_block(uint64_t lba, void *buffer) {
        disk.write_block(lba, super_block.data.block_size, reinterpret_cast<char *>(buffer));
    }

    // HACK:现在这个申请方式就是狗市
    bool allocate_block(uint32_t &lba) {
        spdlog::info("[FileSys] 申请盘块.");
        if (!super_block.data.free_blocks) {
            spdlog::warn("[FileSys] 无空闲盘块.");
            return false;
        }

        uint32_t bitmap_idx;
        uint32_t byte_idx;
        uint32_t bit_idx;

        char buffer[BLOCK_SIZE];

        bool find = false;
        for (bitmap_idx = 0; bitmap_idx < super_block.data.bitmap_blocks_cnt; bitmap_idx++) {
            read_block(bitmap_idx + super_block.data.bitmap_block_start_lba, buffer);
            for (byte_idx = 0; byte_idx < BLOCK_SIZE; byte_idx++) {
                if ((uint8_t)buffer[byte_idx] == 0xff)
                    continue;
                for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                    if (~buffer[byte_idx] & (1 << (7 - bit_idx))) {
                        find = true;
                        break;
                    }
                }
                if (find)
                    break;
            }
            if (find)
                break;
        }

        lba = bitmap_idx * super_block.data.bits_per_block + byte_idx * 8 + bit_idx;
        spdlog::info("[FileSys] 在{}号位图第{}字节第{}位找到空闲位, lba:{}", bitmap_idx, byte_idx,
                     bit_idx, lba);

        spdlog::debug("[FileSys] 虚拟硬盘重新写入Super Block.");
        super_block.data.free_blocks--;
        write_block(0, &super_block);

        spdlog::debug("[FileSys] 更新位图.");
        buffer[byte_idx] |= (1 << (7 - bit_idx));
        write_block(super_block.data.bitmap_block_start_lba + bitmap_idx, buffer);

        return true;
    }

    // FIX: 这对吗?????????
    bool dir_add_item(uint64_t dir_inode_id, uint64_t item_inode_id, std::string item_name) {
        char buffer[BLOCK_SIZE];
        uint32_t dir_inode_block_lba = dir_inode_id / super_block.data.inodes_per_block +
                                       super_block.data.inode_block_start_lba;
        INode dir_inode;
        read_block(dir_inode_block_lba, buffer);
        std::memcpy(&dir_inode,
                    buffer + (dir_inode_id % super_block.data.inodes_per_block) *
                                 super_block.data.inode_size,
                    sizeof(INode));
        // TODO: DIRITEM_SIZE写入Super Block
        if (dir_inode.data.size == super_block.data.block_size / DIRITEM_SIZE)
            return false;

        dir_inode.data.size++;
        std::memcpy(buffer + (dir_inode_id % super_block.data.inodes_per_block) *
                                 super_block.data.inode_size,
                    &dir_inode, sizeof(INode));

        read_block(dir_inode.data.block_lba, buffer);
        DirItem new_item = {
            .inode = item_inode_id,
            .valid = 1,
        };
        // FIX: 使用魔法数 54
        std::memcpy(new_item.name, item_name.data(), std::min(item_name.size(), (size_t)54));
        std::memcpy(buffer + (dir_inode.data.size - 1) * DIRITEM_SIZE, &new_item, DIRITEM_SIZE);
        write_block(dir_inode.data.block_lba, buffer);

        return true;
    }

    bool allocate_inode(uint64_t &inode_id) {
        spdlog::info("[FileSys] 申请空闲INode.");

        if (super_block.data.free_inodes == 0) {
            spdlog::warn("[FileSys] 无空闲INode, 申请失败.");
            return false;
        }

        char buffer[BLOCK_SIZE];
        uint64_t bitmap_idx;
        uint32_t byte_idx;
        uint32_t bit_idx;

        bool find = false;
        for (bitmap_idx = 0; bitmap_idx < super_block.data.inode_valid_blocks_cnt; bitmap_idx++) {
            read_block(bitmap_idx + super_block.data.inode_valid_block_start_lba, buffer);
            for (byte_idx = 0; byte_idx < super_block.data.block_size; byte_idx++) {
                if ((uint8_t)buffer[byte_idx] == 0xff)
                    continue;
                for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                    if (~(uint8_t)buffer[byte_idx] & (1 << (7 - bit_idx))) {
                        find = true;
                        break;
                    }
                }
                if (find)
                    break;
            }
            if (find)
                break;
        }

        inode_id = bitmap_idx * super_block.data.bits_per_block + byte_idx * 8 + bit_idx;

        buffer[byte_idx] |= (1 << (7 - bit_idx));
        write_block(bitmap_idx + super_block.data.inode_valid_block_start_lba, buffer);

        return true;
    }

    bool is_empty_block(uint32_t lba) {
        char buffer[BLOCK_SIZE];
        uint32_t bitmap_lba =
            lba / super_block.data.bits_per_block + super_block.data.bitmap_block_start_lba;
        uint32_t byte_idx = (lba % super_block.data.bits_per_block) / 8;
        uint8_t bit_idx = lba % 8;
        read_block(bitmap_lba, buffer);

        bool is_occupied = buffer[byte_idx] & (1 << (7 - bit_idx));
        return !is_occupied;
    }

private:
    VDisk &disk;
    SuperBlock super_block;
    uint32_t cur_inode;
};
