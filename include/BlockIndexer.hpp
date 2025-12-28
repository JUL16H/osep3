#pragma once
#include "BPTree.hpp"
#include "BlockAllocator.hpp"
#include "IOContext.hpp"
#include "macros.hpp"
#include <cstring>
#include <memory>

class BlockBTreeAdapter : public IBPTreeStorage<uint64_t, uint64_t> {
public:
    BlockBTreeAdapter(std::shared_ptr<IOContext> _ioc, std::shared_ptr<BlockAllocator> _alloc)
        : ioc(_ioc), alloc(_alloc) {}
    void read_node(uint64_t id, std::span<uint8_t> buffer) override { ioc->read_block(id, buffer); }
    void write_node(uint64_t id, std::span<uint8_t> data) override { ioc->write_block(id, data); }
    std::optional<uint64_t> allocate_node() override { return alloc->allocate_block(); }
    void free_node(uint64_t id) override { alloc->free_block(id); }
    void free_val(uint64_t val) override { alloc->free_block(val); }
    size_t get_node_size() const override { return BLOCK_SIZE; }

private:
    std::shared_ptr<IOContext> ioc;
    std::shared_ptr<BlockAllocator> alloc;
};

class BlockIndexer {
    using BlockBTree = BPTree<uint64_t, uint64_t, BLOCK_SIZE>;

public:
    BlockIndexer(std::shared_ptr<SuperBlock> _sb, std::shared_ptr<IOContext> _ioc,
                 std::shared_ptr<BlockAllocator> _blkalloc)
        : sb(_sb), iocontext(_ioc), blkalloc(_blkalloc) {
        auto adapter = std::make_shared<BlockBTreeAdapter>(iocontext, blkalloc);
        btree = std::make_shared<BlockBTree>(adapter);
    }

    std::optional<uint64_t> find_block(uint64_t root_lba, uint64_t file_block_idx) {
        return btree->find(root_lba, file_block_idx);
    }

    std::optional<uint64_t> insert_block(uint64_t root_lba, uint64_t file_block_idx,
                                         uint64_t file_data_lba) {
        return btree->insert(root_lba, file_block_idx, file_data_lba);
    }

    void free_node(uint64_t node_lba) {
        if (node_lba == 0)
            return;
        btree->clear(node_lba);
    }

private:
private:
    std::shared_ptr<SuperBlock> sb;
    std::shared_ptr<IOContext> iocontext;
    std::shared_ptr<BlockAllocator> blkalloc;
    std::shared_ptr<BlockBTree> btree;
};
