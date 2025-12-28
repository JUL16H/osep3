#pragma once
#include <cstdint>

constexpr uint32_t BLOCK_SIZE = 16<<10;

constexpr uint64_t MAGIC_NUMBER = 0xEA6191;
constexpr uint64_t VERSION = 7;

constexpr uint16_t DIRITEM_SIZE = 64;

constexpr uint32_t FILENAME_SIZE = 54;

constexpr uint32_t INODE_SIZE = 512;
constexpr uint32_t INODE_DATA_SIZE = INODE_SIZE - 38;

constexpr uint32_t BTree_M = (BLOCK_SIZE - 16) >> 4;
