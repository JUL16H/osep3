#pragma once
#include "macros.hpp"
#include <cstring>

union SuperBlock {
    struct {
        uint64_t magic_number;
        uint64_t version;

        uint16_t disk_size_gb;
        uint32_t block_size;
        uint64_t total_blocks;
        uint64_t bits_per_block;

        uint64_t super_block_start_lba;
        uint16_t super_blocks_cnt;

        uint64_t bitmap_block_start_lba;
        uint32_t bitmap_blocks_cnt;

        uint64_t inode_size;
        uint32_t inodes_per_block;
        uint64_t inodes_cnt;
        uint64_t free_inodes;
        uint64_t inode_valid_block_start_lba;
        uint64_t inode_valid_blocks_cnt;
        uint64_t inode_block_start_lba;
        uint64_t inode_blocks_cnt;
        uint64_t inode_inline_data_size;

        uint64_t basic_blocks_cnt;

        uint64_t diritem_size;

        uint64_t root_inode_id;
        uint64_t free_blocks;

        uint64_t BTree_M;

        uint32_t bloom_bits;
        uint16_t filename_size;
    } data;
    char padding[BLOCK_SIZE];

    bool valid() {
        return this->data.magic_number == MAGIC_NUMBER && this->data.version == VERSION;
    }
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
    rst.data.bitmap_blocks_cnt =
        (rst.data.bits_per_block + rst.data.total_blocks - 1) / rst.data.bits_per_block;

    rst.data.inode_size = INODE_SIZE;
    rst.data.inode_inline_data_size = INODE_DATA_SIZE;

    rst.data.inodes_per_block = rst.data.block_size / rst.data.inode_size;
    rst.data.inode_valid_block_start_lba =
        rst.data.bitmap_block_start_lba + rst.data.bitmap_blocks_cnt;

    rst.data.inode_blocks_cnt = (((1ull<<30) / rst.data.block_size) >> 7) * rst.data.disk_size_gb;
    rst.data.inodes_cnt = rst.data.inodes_per_block * rst.data.inode_blocks_cnt;
    rst.data.free_inodes = rst.data.inodes_cnt;
    rst.data.inode_valid_blocks_cnt =
        (rst.data.inodes_cnt + rst.data.bits_per_block - 1) / rst.data.bits_per_block;
    rst.data.inode_block_start_lba =
        rst.data.inode_valid_block_start_lba + rst.data.inode_valid_blocks_cnt;


    rst.data.basic_blocks_cnt = rst.data.super_blocks_cnt + rst.data.bitmap_blocks_cnt +
                                rst.data.inode_valid_blocks_cnt + rst.data.inode_blocks_cnt;

    rst.data.diritem_size = DIRITEM_SIZE;

    rst.data.free_blocks = rst.data.total_blocks - rst.data.basic_blocks_cnt;

    rst.data.BTree_M = BTree_M;

    rst.data.filename_size = FILENAME_SIZE;

    return rst;
}
