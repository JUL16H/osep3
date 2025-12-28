#pragma once
#include "IOContext.hpp"
#include <optional>

class BlockAllocator {
public:
    BlockAllocator(std::shared_ptr<SuperBlock> _sb, std::shared_ptr<IOContext> _ioc)
        : sb(_sb), iocontext(_ioc) {}

    void reset_bitmap() {
        spdlog::debug("[Bitmap Manager] 写入位图.");
        std::shared_ptr<Buffer> buffer;

        uint64_t full_bitmap_blocks = sb->data.basic_blocks_cnt / sb->data.bits_per_block;
        uint64_t remaining_blocks = sb->data.basic_blocks_cnt % sb->data.bits_per_block;
        uint64_t remaining_bytes = remaining_blocks / 8;
        uint64_t remaining_bits = remaining_blocks % 8;

        // 全1 Bitmap Block
        for (uint64_t i = 0; i < full_bitmap_blocks; i++) {
            buffer = iocontext->acquire_block(i + sb->data.bitmap_block_start_lba);
            std::ranges::fill(*buffer, 0xff);
        }

        //  部分1 Bitmap Block
        buffer = iocontext->acquire_block(full_bitmap_blocks + sb->data.inode_block_start_lba);
        for (uint64_t i = 0; i < remaining_bytes; i++)
            (*buffer)[i] = 0xff;
        if (remaining_bits)
            (*buffer)[remaining_bytes] |= (0xff << (8 - remaining_bits));

        // 全0 BitMap Block
        for (uint64_t i = full_bitmap_blocks + (remaining_bytes || remaining_bits);
             i < sb->data.bitmap_blocks_cnt; i++) {
            buffer = iocontext->acquire_block(i + sb->data.bitmap_block_start_lba);
            std::ranges::fill(*buffer, 0);
        }
        spdlog::debug("[Bitmap Manager] 完成位图写入.");
    }

    std::optional<uint64_t> allocate_block() {
        spdlog::debug("[BitmapManager] 查找空闲盘块.");

        bool find = false;
        uint64_t bitmap_block_idx, byte_idx;
        uint8_t bit_idx;

        std::shared_ptr<const Buffer> cur_buffer;
        for (bitmap_block_idx = 0; bitmap_block_idx < sb->data.bitmap_blocks_cnt;
             bitmap_block_idx++) {
            cur_buffer = iocontext->read_block(bitmap_block_idx + sb->data.bitmap_block_start_lba);
            for (byte_idx = 0; byte_idx < sb->data.block_size; byte_idx++) {
                if ((*cur_buffer)[byte_idx] != 0xff) {
                    find = true;
                    for (bit_idx = 0; bit_idx < 8; bit_idx++) {
                        if (~(*cur_buffer)[byte_idx] & (1 << (7 - bit_idx)))
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
            return std::nullopt;
        }

        std::shared_ptr<Buffer> buffer =
            iocontext->acquire_block(bitmap_block_idx + sb->data.bitmap_block_start_lba);
        (*buffer)[byte_idx] |= (1 << (7 - bit_idx));
        uint64_t lba = bitmap_block_idx * sb->data.bits_per_block + byte_idx * 8 + bit_idx;
        spdlog::debug("[BitmapManager] 找到空闲盘块, LBA: 0x{:X}", lba);
        sb->data.free_blocks--;
        return lba;
    }

    void free_block(uint64_t lba) {
        uint64_t bitmap_lba = lba / sb->data.bits_per_block + sb->data.bitmap_block_start_lba;
        uint64_t byte_idx = (lba % sb->data.bits_per_block) / 8;
        uint64_t bit_idx = lba % 8;

        std::shared_ptr<Buffer>buffer = iocontext->acquire_block(bitmap_lba);
        (*buffer)[byte_idx] &= ~((uint8_t)1 << (7 - bit_idx));

        sb->data.free_blocks++;
    }

private:
    std::shared_ptr<SuperBlock> sb;
    std::shared_ptr<IOContext> iocontext;
};
