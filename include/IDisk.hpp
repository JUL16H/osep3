#pragma once
#include <cstdint>

class IDisk {
public:
    IDisk(uint32_t _disk_size, uint32_t _block_size)
        : disk_size_gb(_disk_size), block_size(_block_size) {}
    virtual ~IDisk() {}
    virtual void clear() = 0;
    virtual void read_block(uint64_t lba, char *buffer) = 0;
    virtual void write_block(uint64_t lba, const char *data) = 0;
    virtual void flush() = 0;

    void set_block_size(uint32_t _block_size) { block_size = _block_size; }
    uint32_t get_disk_size() { return disk_size_gb; }

protected:
    uint64_t get_expected_size_bytes() const { return (uint64_t)disk_size_gb * (1ULL << 30); }

protected:
    uint32_t disk_size_gb;
    uint32_t block_size;
};
