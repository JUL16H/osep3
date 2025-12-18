#pragma once
#include "SuperBlock.hpp"
#include "VDisk.hpp"
#include "macros.hpp"

class BlockManager {
public:
    BlockManager(VDisk &_disk, SuperBlock &sb) : disk(_disk), super_block(sb) {}

    void read_block(uint64_t lba, void *buffer) {
        disk.read_block(lba, reinterpret_cast<char *>(buffer));
    }

    void write_block(uint64_t lba, const void *data) {
        disk.write_block(lba, reinterpret_cast<const char *>(data));
    }

    // HACK:现在这个申请方式就是狗市
    bool allocate_block(uint64_t &lba) {
        spdlog::info("[FileSys] 申请盘块.");
        if (!super_block.data.free_blocks) {
            spdlog::warn("[FileSys] 无空闲盘块.");
            return false;
        }

        uint64_t bitmap_idx;
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

        spdlog::debug("[FileSys] 更新INode位图.");
        buffer[byte_idx] |= (1 << (7 - bit_idx));
        write_block(bitmap_idx + super_block.data.inode_valid_block_start_lba, buffer);

        spdlog::debug("[FileSys] 更新Super Block.");
        super_block.data.free_inodes--;
        write_block(super_block.data.super_block_start_lba, &super_block);

        return true;
    }

    void clear() {
        disk.clear();
    }

    auto get_disk_size() {
        return disk.get_disk_size();
    }

private:
    VDisk &disk;
    SuperBlock &super_block;
};
