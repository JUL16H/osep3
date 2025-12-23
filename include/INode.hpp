#pragma once
#include "macros.hpp"
#include <cstdint>
#include <cstring>

enum class FileType : uint8_t {
    File = 0,
    Directory = 1,
};

enum class StorageType : uint8_t {
    Inline = 0,
    Direct = 1,
    BTree = 2,
};

struct INode {
    INode(uint64_t _ID = 0, uint64_t _prev_inode_id = 0) {
        std::memset(this, 0, INODE_SIZE);
        ID = _ID;
        prev_inode_id = _prev_inode_id;
    }
    uint64_t ID;
    uint64_t prev_inode_id;
    uint64_t block_lba;
    uint32_t link_cnt;
    FileType file_type;
    StorageType storage_type;
    char inline_data[INODE_DATA_SIZE];
    uint64_t size;
};
static_assert(sizeof(INode) == INODE_SIZE);
