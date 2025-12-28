#pragma once
#include "IDisk.hpp"
#include "LRUCache.hpp"
#include "SuperBlock.hpp"
#include <memory>
#include <spdlog/spdlog.h>

typedef std::vector<uint8_t> Buffer;

class BlockCacheBackend : public ICacheBackend<uint64_t, std::vector<uint8_t>> {
public:
    BlockCacheBackend(std::shared_ptr<SuperBlock> _sb, std::shared_ptr<IDisk> _disk)
        : sb(_sb), disk(_disk) {}

    std::vector<uint8_t> load(uint64_t lba) override {
        std::vector<uint8_t> buffer(sb->data.block_size);
        if (lba == 0)
            std::ranges::fill(buffer, 0);
        else
            disk->read_block(lba, reinterpret_cast<char *>(buffer.data()));
        return buffer;
    };

    void save(uint64_t lba, const std::vector<uint8_t> &buffer) override {
        if (lba == 0)
            return;
        disk->write_block(lba, reinterpret_cast<const char *>(buffer.data()));
    }

private:
    std::shared_ptr<SuperBlock> sb;
    std::shared_ptr<IDisk> disk;
};

class IOContext {
public:
    IOContext(std::shared_ptr<SuperBlock> _sb, std::shared_ptr<IDisk> _disk,
              uint32_t _cache_size = 16384)
        : sb(_sb), disk(_disk) {
        auto backend = std::make_shared<BlockCacheBackend>(sb, disk);
        cache = std::make_unique<LRUCache<uint64_t, std::vector<uint8_t>>>(_cache_size, backend);
    }

    ~IOContext() { flush_all(); }

    void flush_all() {
        flush_super_block();
        cache->flush_all();
    }

    void read_super_block() { disk->read_block(0, reinterpret_cast<char *>(sb.get())); }
    void flush_super_block() { disk->write_block(0, reinterpret_cast<char *>(sb.get())); }

    std::shared_ptr<const std::vector<uint8_t>> read_block(uint64_t lba) {
        if (lba == 0)
            return nullptr;
        return cache->get(lba);
    }

    std::shared_ptr<std::vector<uint8_t>> acquire_block(uint64_t lba) {
        if (lba == 0)
            return nullptr;
        return cache->get_mut(lba);
    }

    void clear() {
        cache->clear();
        disk->clear();
    }

private:
    std::shared_ptr<IDisk> disk;
    std::shared_ptr<SuperBlock> sb;
    std::unique_ptr<LRUCache<uint64_t, std::vector<uint8_t>>> cache;
};
