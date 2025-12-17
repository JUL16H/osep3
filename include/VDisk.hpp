#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>

class VDisk {
private:
    std::fstream fileStream;
    uint32_t disk_size_gb;
    std::string disk_path;

    void open_stream() {
        fileStream.open(disk_path, std::ios::in | std::ios::out | std::ios::binary);
        if (!fileStream) {
            spdlog::critical("[VDisk] 打开虚拟硬盘文件失败: {}", disk_path);
            throw std::runtime_error("无法打开虚拟硬盘文件！");
        }
    }

    uint64_t get_expected_size_bytes() const { return (uint64_t)disk_size_gb * (1ULL << 30); }

public:
    VDisk(uint32_t _disk_size, std::string _disk_path)
        : disk_size_gb(_disk_size), disk_path(_disk_path) {

        bool need_init = false;

        fileStream.open(disk_path, std::ios::in | std::ios::out | std::ios::binary);

        if (fileStream.is_open()) {
            try {
                uint64_t current_size = std::filesystem::file_size(disk_path);
                uint64_t expected_size = get_expected_size_bytes();

                if (current_size != expected_size) {
                    spdlog::warn("[VDisk] 硬盘大小不匹配. 现有: {} B, 期望: {} B ({} GB).",
                                 current_size, expected_size, disk_size_gb);

                    fileStream.close();
                    clear();
                    return;
                } else {
                    spdlog::info("[VDisk] 成功加载现有虚拟硬盘.", disk_path);
                }
            } catch (const std::filesystem::filesystem_error &e) {
                spdlog::error("[VDisk] 获取文件大小时出错: {}", e.what());
                fileStream.close();
                need_init = true;
            }
        } else {
            need_init = true;
        }

        if (need_init) {
            spdlog::info("[VDisk] 初始化新虚拟硬盘.");
            std::ofstream tempCreate(disk_path, std::ios::out | std::ios::binary);
            tempCreate.close();
            clear();
        }
    }

    ~VDisk() {
        if (fileStream.is_open()) {
            fileStream.close();
        }
    }

    void clear() {
        spdlog::info("[VDisk] 清空虚拟硬盘.");

        if (fileStream.is_open()) {
            fileStream.close();
        }

        try {
            std::filesystem::resize_file(disk_path, 0);
            std::filesystem::resize_file(disk_path, get_expected_size_bytes());
        } catch (const std::filesystem::filesystem_error &e) {
            try {
                open_stream();
            } catch (...) {
            }
            throw std::runtime_error(std::string("硬盘清空失败: ") + e.what());
        }

        open_stream();
    }

    void write_block(uint64_t lba, uint32_t block_size, const char *data) {
        if (lba >= get_expected_size_bytes() / block_size) {
            throw std::out_of_range("LBA 超出硬盘范围");
        }

        uint64_t offset = lba * block_size;

        fileStream.clear();
        fileStream.seekp(offset, std::ios::beg);
        fileStream.write(data, block_size);
        fileStream.flush();
    }

    void read_block(uint64_t lba, uint32_t block_size, char *buffer) {
        if (lba >= get_expected_size_bytes() / block_size) {
            throw std::out_of_range("LBA 超出硬盘范围");
        }

        uint64_t offset = lba * block_size;

        fileStream.clear();
        fileStream.seekg(offset, std::ios::beg);

        if (!fileStream.read(buffer, block_size)) {
            if (fileStream.eof() ||
                fileStream.gcount() < static_cast<std::streamsize>(block_size)) {
                std::memset(buffer + fileStream.gcount(), 0, block_size - fileStream.gcount());
                fileStream.clear();
            } else {
                spdlog::error("[VDisk] 读取硬盘失败 LBA: 0x{:X}.", lba);
            }
        }
    }

    uint32_t get_disk_size() { return disk_size_gb; }
};
