#pragma once
#include "SuperBlock.hpp"
#include "VDisk.hpp"
#include "macros.hpp"

class BlockManager {
public:
    BlockManager(IDisk &_disk, SuperBlock &sb) : disk(_disk), super_block(sb) {}

    void refresh_super_block() {
        spdlog::debug("[BlockManager] Super Block 信息写入硬盘.");
        disk.write_block(0, reinterpret_cast<char*>(&super_block));
    }

    void read_super_block() {
        spdlog::debug("[BlockManager] 从硬盘中读取 Super Block 信息.");
        disk.read_block(0, reinterpret_cast<char*>(&super_block));
    }

    void read_block(uint64_t lba, std::span<uint8_t> buffer) {
        spdlog::debug("[BlockManager] 读取盘块, LBA: 0x{:X}", lba);
        if (buffer.size() != super_block.data.block_size) {
            spdlog::critical("[BlockManager] buffer大小与盘块大小不符.");
            throw std::runtime_error("buffer大小与盘块大小不符.");
        }
        disk.read_block(lba, reinterpret_cast<char*>(buffer.data()));
    }

    void write_block(uint64_t lba, std::span<uint8_t> buffer) {
        spdlog::debug("[BlockManager] 写入盘块, LBA: 0x{:X}", lba);
        if (buffer.size() != super_block.data.block_size) {
            spdlog::critical("[BlockManager] buffer大小与盘块大小不符.");
            throw std::runtime_error("buffer大小与盘块大小不符.");
        }
        disk.write_block(lba, reinterpret_cast<char*>(buffer.data()));
    }

    void clear() {
        spdlog::debug("[BlockManager] 重置硬盘.");
        disk.clear();
    }

private:
    IDisk &disk;
    SuperBlock &super_block;
};
