#pragma once
#include "IDisk.hpp"
#include "SuperBlock.hpp"
#include <list>
#include <memory>
#include <spdlog/spdlog.h>
#include <unordered_map>

class IOContext {
    struct CacheItem {
        uint64_t lba;
        std::vector<uint8_t> buffer;
        bool dirty = false;
    };

public:
    IOContext(std::shared_ptr<SuperBlock> _sb, std::shared_ptr<IDisk> _disk,
              uint32_t _cache_size = 16384)
        : sb(_sb), disk(_disk), max_cache_size(_cache_size) {
        spdlog::debug("[IOContext] 从硬盘中读取 Super Block 信息.");
    }

    ~IOContext() { flush_all(); }

    void flush_all() {
        flush_super_block();
        for (auto &it : cache_list)
            if (it.dirty)
                disk->write_block(it.lba, reinterpret_cast<char *>(it.buffer.data()));
    }

    void flush_super_block() {
        spdlog::trace("[IOContext] Super Block 信息写入硬盘.");
        disk->write_block(0, reinterpret_cast<char *>(sb.get()));
    }

    void read_super_block() {
        spdlog::trace("[IOContext] 从硬盘读取 Super Block 信息.");
        disk->read_block(0, reinterpret_cast<char *>(sb.get()));
    }

    void read_block(uint64_t lba, std::span<uint8_t> buffer) {
        if (lba == 0) {
            std::ranges::fill(buffer, 0);
            return;
        }
        spdlog::trace("[IOContext] 读取盘块, LBA: 0x{:X}", lba);
        if (buffer.size() != sb->data.block_size) {
            spdlog::critical("[IOContext] buffer大小与盘块大小不符.");
            throw std::runtime_error("buffer大小与盘块大小不符.");
        }
        auto it = get(lba);
        // Can be: buffer = it->buffer; ?
        std::memcpy(buffer.data(), it->buffer.data(), sb->data.block_size);
    }

    void write_block(uint64_t lba, std::span<uint8_t> data) {
        if (lba == 0)
            return;
        spdlog::trace("[IOContext] 写入盘块, LBA: 0x{:X}", lba);
        if (data.size() != sb->data.block_size) {
            spdlog::critical("[IOContext] buffer大小与盘块大小不符.");
            throw std::runtime_error("buffer大小与盘块大小不符.");
        }
        auto it = get(lba);
        it->dirty = true;
        std::memcpy(it->buffer.data(), data.data(), sb->data.block_size);
    }

    void clear() {
        spdlog::debug("[IOContext] 重置硬盘.");
        cache_list.clear();
        cache_mp.clear();
        disk->clear();
    }

private:
    std::list<CacheItem>::iterator get(uint64_t lba) {
        if (cache_mp.count(lba)) {
            this->cache_list.splice(cache_list.begin(), cache_list, cache_mp[lba]);
            return cache_mp[lba];
        }
        if (cache_list.size() < max_cache_size) {
            std::vector<uint8_t> buffer(sb->data.block_size);
            disk->read_block(lba, reinterpret_cast<char *>(buffer.data()));
            cache_list.push_front(CacheItem{.lba = lba, .buffer = buffer});
            cache_mp[lba] = cache_list.begin();
            return cache_mp[lba];
        }

        CacheItem old_item = cache_list.back();
        if (old_item.dirty)
            disk->write_block(old_item.lba, reinterpret_cast<char *>(old_item.buffer.data()));
        cache_list.pop_back();
        cache_mp.erase(old_item.lba);

        std::vector<uint8_t> buffer(sb->data.block_size);
        disk->read_block(lba, reinterpret_cast<char *>(buffer.data()));
        cache_list.push_front(CacheItem{.lba = lba, .buffer = buffer});
        cache_mp[lba] = cache_list.begin();
        return cache_mp[lba];
    }

private:
    std::shared_ptr<IDisk> disk;
    std::shared_ptr<SuperBlock> sb;

    const uint32_t max_cache_size;
    std::list<CacheItem> cache_list;
    std::unordered_map<uint64_t, typename decltype(cache_list)::iterator> cache_mp;
};
