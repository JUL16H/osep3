#pragma once
#include "IDisk.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>

class VDisk : public IDisk {
public:
    VDisk(uint32_t _disk_size, uint32_t _block_size, std::string _disk_path)
        : IDisk(_disk_size, _block_size), disk_path(_disk_path) {

        spdlog::info("[VDisk] 尝试打开虚拟硬盘.");
        file.open(disk_path, std::ios::in | std::ios::out | std::ios::binary);

        if (file.is_open()) {
            spdlog::info("[VDisk] 虚拟硬盘打开成功.");
            try {
                uint64_t current_size = std::filesystem::file_size(disk_path);
                uint64_t expected_size = get_expected_size_bytes();

                if (current_size != expected_size) {
                    spdlog::warn("[VDisk] 虚拟硬盘大小不匹配. 现有: {} B, 期望: {} B ({} GB).",
                                 current_size, expected_size, disk_size_gb);

                    file.close();
                    clear();
                    return;
                } else {
                    spdlog::info("[VDisk] 成功加载现有虚拟硬盘.", disk_path);
                }
            } catch (const std::filesystem::filesystem_error &e) {
                spdlog::error("[VDisk] 获取文件大小时出错: {}", e.what());
                file.close();
            }
        } else {
            spdlog::info("[VDisk] 打开失败，初始化新虚拟硬盘.");
            std::ofstream tempCreate(disk_path, std::ios::out | std::ios::binary);
            tempCreate.close();
            clear();
        }
    }

    ~VDisk() {
        spdlog::info("[VDisk] VDisk层退出.");
        flush();
        if (file.is_open()) {
            spdlog::info("[VDisk] 关闭虚拟硬盘.");
            file.close();
        }
    }

    void clear() override {
        spdlog::info("[VDisk] 清空虚拟硬盘.");

        if (file.is_open()) {
            file.close();
        }

        // try {
        std::filesystem::resize_file(disk_path, 0);
        std::filesystem::resize_file(disk_path, get_expected_size_bytes());
        // } catch (const std::filesystem::filesystem_error &e) {
        //     try {
        //         open_stream();
        //     } catch (...) {
        //     }
        //     throw std::runtime_error(std::string("硬盘清空失败: ") + e.what());
        // }

        open_stream();
    }

    void read_block(uint64_t lba, char *buffer) override {
        spdlog::debug("[VDisk] 从虚拟硬盘读取盘块. LBA: 0x{:X}.", lba);
        if (lba >= get_expected_size_bytes() / block_size) {
            throw std::out_of_range("LBA 超出虚拟硬盘范围");
        }

        uint64_t offset = lba * block_size;

        file.clear();
        file.seekg(offset, std::ios::beg);

        if (!file.read(buffer, block_size)) {
            if (file.eof() || file.gcount() < static_cast<std::streamsize>(block_size)) {
                std::memset(buffer + file.gcount(), 0, block_size - file.gcount());
                file.clear();
            } else {
                spdlog::error("[VDisk] 读取虚拟硬盘失败 LBA: 0x{:X}.", lba);
            }
        }
    }

    void write_block(uint64_t lba, const char *data) override {
        spdlog::debug("[VDisk] 向虚拟硬盘写入盘块. LBA: 0x{:X}.", lba);
        if (lba >= get_expected_size_bytes() / block_size) {
            throw std::out_of_range("LBA 超出虚拟硬盘范围");
        }

        uint64_t offset = lba * block_size;

        file.clear();
        file.seekp(offset, std::ios::beg);
        file.write(data, block_size);
    }

    void flush() override { file.flush(); }

private:
    void open_stream() {
        file.open(disk_path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file) {
            spdlog::critical("[VDisk] 打开虚拟硬盘文件失败: {}", disk_path);
            throw std::runtime_error("无法打开虚拟硬盘文件.");
        }
    }

private:
    std::fstream file;
    std::string disk_path;
};
