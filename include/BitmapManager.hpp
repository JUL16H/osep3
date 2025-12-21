#pragma once
#include "BlockManager.hpp"
#include "macros.hpp"

class BitmapManager {
public:
    BitmapManager(SuperBlock &sb, BlockManager &bm) : super_block(sb), block_manager(bm) {}

    void reset_bitmap() {
        spdlog::debug("[Bitmap Manager] 写入位图.");
        std::vector<uint8_t> buffer(super_block.data.block_size);

        uint64_t full_bitmap_blocks =
            super_block.data.basic_blocks_cnt / super_block.data.bits_per_block;
        uint64_t remaining_blocks =
            super_block.data.basic_blocks_cnt % super_block.data.bits_per_block;
        uint64_t remaining_bytes = remaining_blocks / 8;
        uint64_t remaining_bits = remaining_blocks % 8;

        // 全1 Bitmap Block
        std::ranges::fill(buffer, 0xff);
        for (uint64_t i = 0; i < full_bitmap_blocks; i++)
            block_manager.write_block(i + super_block.data.bitmap_block_start_lba, buffer);

        //  部分1 Bitmap Block
        std::ranges::fill(buffer, 0);
        for (uint64_t i = 0; i < remaining_bytes; i++)
            buffer[i] = 0xff;

        if (remaining_bits)
            buffer[remaining_bytes] |= (0xff << (8 - remaining_bits));
        block_manager.write_block(super_block.data.bitmap_block_start_lba + full_bitmap_blocks,
                                  buffer);

        // 全0 BitMap Block
        std::ranges::fill(buffer, 0);
        for (uint64_t i = full_bitmap_blocks + (remaining_bytes || remaining_bits);
             i < super_block.data.bitmap_blocks_cnt; i++)
            block_manager.write_block(i + super_block.data.bitmap_block_start_lba, buffer);
        spdlog::debug("[Bitmap Manager] 完成位图写入.");
    }

    bool allocate_block(uint64_t &lba) {
        spdlog::debug("[BitmapManager] 查找空闲盘块.");

        bool find = false;
        uint64_t bitmap_block_idx, byte_idx;
        uint8_t bit_idx;
        std::vector<uint8_t> buffer(super_block.data.block_size);

        for (bitmap_block_idx = 0; bitmap_block_idx < super_block.data.bitmap_blocks_cnt;
             bitmap_block_idx++) {
            block_manager.read_block(bitmap_block_idx + super_block.data.bitmap_block_start_lba,
                                     buffer);
            for (byte_idx = 0; byte_idx < super_block.data.block_size; byte_idx++) {
                if (buffer[byte_idx] != 0xff) {
                    find = true;
                    for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                        if (~buffer[byte_idx] & (1 << (7 - bit_idx)))
                            break;
                    }
                }
                if (find)
                    break;
            }
            if (find)
                break;
        }
        if (!find) {
            spdlog::warn("[BitmapManager] 未找到空闲盘块.");
            return false;
        }

        buffer[byte_idx] |= (1 << (7 - bit_idx));
        block_manager.write_block(bitmap_block_idx + super_block.data.bitmap_block_start_lba,
                                  buffer);
        lba = bitmap_block_idx * super_block.data.bits_per_block + byte_idx * 8 + bit_idx;
        spdlog::debug("[BitmapManager] 找到空闲盘块, LBA: 0x{:X}", lba);
        return true;
    }

private:
    const SuperBlock &super_block;
    BlockManager &block_manager;
};
