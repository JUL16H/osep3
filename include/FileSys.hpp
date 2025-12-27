#pragma once
#include "BlockAllocator.hpp"
#include "IDisk.hpp"
#include "INode.hpp"
#include "INodeTable.hpp"
#include "IOContext.hpp"
#include "SuperBlock.hpp"
#include <cstdint>
#include <format>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>

struct FileHandle {
    uint64_t inode_id;
    uint64_t offset;
};

class FileSys {
    friend int main(); // HACK: Just added when DEBUG.
public:
    FileSys(IDisk *_disk) : disk(_disk) {
        spdlog::info("[FileSys] 文件系统启动.");
        sb = std::make_shared<SuperBlock>();
        iocontext = std::make_shared<IOContext>(sb, disk);
        blkalloc = std::make_shared<BlockAllocator>(sb, iocontext);
        blkidxer = std::make_shared<BlockIndexer>(sb, iocontext, blkalloc);
        inodetable = std::make_shared<INodeTable>(sb, iocontext, blkalloc, blkidxer);

        iocontext->read_super_block();

        if (!sb->valid()) {
            spdlog::info("[FileSys] 文件系统不匹配, 执行硬盘格式化.");
            format();
            spdlog::info("[FileSys] 重新读取Super Block.");
            iocontext->read_super_block();
        }

        debug_super_block_info();
    }
    ~FileSys() {}

    void debug_super_block_info() {
        spdlog::debug("[FileSys] 硬盘Super Block信息:");
        spdlog::debug("[FileSys] Magic Number: 0x{:X}.", sb->data.magic_number);
        spdlog::debug("[FileSys] Version: {}.", sb->data.version);
        spdlog::debug("[FileSys] Disk Size: {} GB.", sb->data.disk_size_gb);
        spdlog::debug("[FileSys] Block Size: {} B.", sb->data.block_size);
        spdlog::debug("[FileSys] Total Blocks: {}.", sb->data.total_blocks);
        spdlog::debug("[FileSys] Super Block Start LBA: 0x{:X}.", sb->data.super_block_start_lba);
        spdlog::debug("[FileSys] Super Blocks Count: {}.", sb->data.super_blocks_cnt);
        spdlog::debug("[FileSys] Bitmap Block Start LBA: 0x{:x}.", sb->data.bitmap_block_start_lba);
        spdlog::debug("[FileSys] Bitmap Blocks Count: {}.", sb->data.bitmap_blocks_cnt);
        spdlog::debug("[FileSys] INode Size: {} B.", sb->data.inode_size);
        spdlog::debug("[FileSys] INodes Count: {}.", sb->data.inodes_cnt);
        spdlog::debug("[FileSys] INode Valid Block Start LBA: 0x{:X}.",
                      sb->data.inode_valid_block_start_lba);
        spdlog::debug("[FileSys] INode Valid Blocks Count: {}.", sb->data.inode_valid_blocks_cnt);
        spdlog::debug("[FileSys] INode Block Start LBA: 0x{:X}.", sb->data.inode_block_start_lba);
        spdlog::debug("[FileSys] INode Blocks Count: {}.", sb->data.inode_blocks_cnt);
        spdlog::debug("[FileSys] Basic Blocks Count: {}.", sb->data.basic_blocks_cnt);
        spdlog::debug("[FileSys] Root INode: 0x{:X}.", sb->data.root_inode_id);
        spdlog::debug("[FileSys] Free Blocks: {}.", sb->data.free_blocks);
    }

    void format() {
        spdlog::info("[FileSys] 进行硬盘格式化.");

        spdlog::debug("[FileSys] 清空硬盘.");
        iocontext->clear();
        inodetable->clear_cache();

        spdlog::debug("[FileSys] 写入Super Block.");
        *sb = create_superblock(disk->get_disk_size());
        iocontext->flush_super_block();

        spdlog::debug("[FileSys] 写入bitmap.");
        blkalloc->reset_bitmap();

        spdlog::debug("[FileSys] 写入INode bitmap.");
        inodetable->reset_inode_bitmap();

        spdlog::debug("[FileSys] 创建根目录.");
        create_root_dir();

        spdlog::info("[FileSys] 格式化完成.");
    }

    bool create_dir(std::string path, std::string name) {
        spdlog::info("[FileSys] 创建目录 path:{}, name:{}.", path, name);

        auto path_id = lookup_path(path);
        if (!path_id)
            return false;

        auto dir_id = inodetable->allocate_inode(FileType::Directory);
        if (!dir_id)
            return false;

        inodetable->add_diritem(dir_id.value(), ".", dir_id.value());
        inodetable->add_diritem(dir_id.value(), "..", path_id.value());
        inodetable->add_diritem(path_id.value(), name, dir_id.value());
        return true;
    }

    void list_directory(std::string path) {
        spdlog::info("[FileSys] 列出目录项 path:{}.", path);
        auto node_id = lookup_path(path);
        if (!node_id)
            return;
        INode node = inodetable->get_inode_info(node_id.value());
        const uint64_t epoch_num = 1024;
        for (uint64_t offset = 0; offset < node.size; offset += epoch_num * sb->data.diritem_size) {
            std::vector<uint8_t> buffer(epoch_num * sb->data.diritem_size);
            auto size = inodetable->read_data(node_id.value(), offset, buffer);
            buffer.resize(size);
            for (auto i = 0; i < buffer.size(); i += sb->data.diritem_size) {
                DirItem *item = reinterpret_cast<DirItem *>(buffer.data() + i);
                INode node = inodetable->get_inode_info(item->inode_id);
                std::cout << std::format("{} {} {}\n", item->inode_id, node.size, item->name);
            }
            std::cout << "\n";
        }
    }

    bool create_file(std::string path, std::string name) {
        spdlog::info("[FileSys] 创建文件 path:{}, name:{}.", path, name);

        auto path_id = lookup_path(path);
        if (!path_id)
            return false;

        auto new_file_id = inodetable->allocate_inode(FileType::File);
        if (!new_file_id)
            return false;

        inodetable->add_diritem(path_id.value(), name, new_file_id.value());
        return true;
    }

    std::optional<uint64_t> open(std::string path, uint64_t offset = 0) {
        auto inode_id_opt = lookup_path(path);
        if (!inode_id_opt) {
            return std::nullopt;
        }
        auto inode_id = inode_id_opt.value();

        INode node = inodetable->get_inode_info(inode_id);
        if (node.file_type != FileType::File) {
            return std::nullopt;
        }

        uint64_t fd = cur_fd++;
        fd_table[fd] = FileHandle{.inode_id = inode_id, .offset = offset};

        return fd;
    }

    void close(uint64_t fd) {
        if (fd_table.find(fd) == fd_table.end())
            return;
        fd_table.erase(fd);
    }

    bool write(uint64_t fd, std::span<uint8_t> data) {
        if (!fd_table.count(fd))
            return false;
        auto &handle = fd_table[fd];

        bool success = inodetable->write_data(handle.inode_id, handle.offset, data);
        if (success) {
            handle.offset += data.size();
        }
        return success;
    }

    size_t read(uint64_t fd, std::span<uint8_t> buffer) {
        if (!fd_table.count(fd))
            return 0;
        auto &handle = fd_table[fd];
        auto size = inodetable->read_data(handle.inode_id, handle.offset, buffer);
        handle.offset += size;
        return size;
    }

    void seek(uint64_t fd, uint64_t offset) {
        if (!fd_table.count(fd))
            return;
        fd_table[fd].offset = offset;
    }

    bool has_dir(std::string path) {
        auto inode_id_opt = lookup_path(path);
        if (!inode_id_opt)
            return false;
        INode inode = inodetable->get_inode_info(inode_id_opt.value());
        return inode.file_type == FileType::Directory;
    }

private:
    void create_root_dir() {
        spdlog::info("[FileSys] 创建根目录.");
        sb->data.root_inode_id = inodetable->allocate_inode(FileType::Directory).value();
        inodetable->add_diritem(sb->data.root_inode_id, ".", sb->data.root_inode_id);
        inodetable->add_diritem(sb->data.root_inode_id, "..", sb->data.root_inode_id);
    }

    std::optional<uint64_t> lookup_path(std::string_view path) {
        if (path[0] != '/')
            return std::nullopt;
        path = path.substr(1);
        uint64_t cur_node_id = sb->data.root_inode_id;

        std::string name;
        while (path.size()) {
            auto idx = path.find('/');
            if (idx == std::string::npos) {
                name = path;
                path = "";
            } else {
                name = path.substr(0, idx);
                path = path.substr(idx + 1);
            }
            auto optid = inodetable->find_inode_by_name(cur_node_id, name);
            if (!optid)
                return std::nullopt;
            cur_node_id = optid.value();
        }
        return cur_node_id;
    }

private:
    std::shared_ptr<IDisk> disk;
    std::shared_ptr<SuperBlock> sb;
    std::shared_ptr<IOContext> iocontext;
    std::shared_ptr<BlockAllocator> blkalloc;
    std::shared_ptr<INodeTable> inodetable;
    std::shared_ptr<BlockIndexer> blkidxer;

    uint64_t cur_fd = 0;
    std::unordered_map<uint64_t, FileHandle> fd_table;
};
