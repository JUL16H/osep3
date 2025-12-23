#pragma once
#include "Singleton.hpp"
#include "SuperBlock.hpp"
#include "VDisk.hpp"
#include "macros.hpp"
#include <memory>

class BlockManager : public Singleton<BlockManager> {
    friend class Singleton;

public:
    void set_super_block(std::shared_ptr<SuperBlock> sb) { super_block = sb; }
    void set_disk(std::shared_ptr<IDisk> _disk) { disk = _disk; }
    void refresh_super_block() {
        spdlog::debug("[BlockManager] Super Block 信息写入硬盘.");
        disk->write_block(0, reinterpret_cast<char *>(super_block.get()));
    }

    void read_super_block() {
        spdlog::debug("[BlockManager] 从硬盘中读取 Super Block 信息.");
        disk->read_block(0, reinterpret_cast<char *>(super_block.get()));
    }

    void read_block(uint64_t lba, std::span<uint8_t> buffer) {
        spdlog::debug("[BlockManager] 读取盘块, LBA: 0x{:X}", lba);
        if (buffer.size() != super_block->data.block_size) {
            spdlog::critical("[BlockManager] buffer大小与盘块大小不符.");
            throw std::runtime_error("buffer大小与盘块大小不符.");
        }
        disk->read_block(lba, reinterpret_cast<char *>(buffer.data()));
    }

    void write_block(uint64_t lba, std::span<uint8_t> buffer) {
        spdlog::debug("[BlockManager] 写入盘块, LBA: 0x{:X}", lba);
        if (buffer.size() != super_block->data.block_size) {
            spdlog::critical("[BlockManager] buffer大小与盘块大小不符.");
            throw std::runtime_error("buffer大小与盘块大小不符.");
        }
        disk->write_block(lba, reinterpret_cast<char *>(buffer.data()));
    }

    void clear() {
        spdlog::debug("[BlockManager] 重置硬盘.");
        disk->clear();
    }

private:
    std::shared_ptr<IDisk> disk;
    std::shared_ptr<SuperBlock> super_block;
};
