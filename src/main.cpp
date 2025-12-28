#include "CLI.hpp"
#include "FileDisk.hpp"
#include "FileSys.hpp"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <cassert>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <span>

// ================= 日志初始化 =================
void init_logger() {
    // 创建一个轮转日志，最大 5MB，保留 5 个文件
    auto logger = spdlog::rotating_logger_mt("basic_logger", "log.log", 5ULL << 20, 5);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    // 默认级别设为 debug，但在 main 中我们会为了压力测试调整为 warn
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_on(spdlog::level::trace);
}

// ================= 配置区域 =================
const std::string DISK_PATH = "vdisk_4tb.img";
const uint32_t DISK_SIZE_GB = 4096; // 4TB
const size_t CHUNK_SIZE = 1 * 1024 * 1024; // 1MB 内存缓冲区
// ===========================================

// 工具：生成随机 Buffer
void fill_random(std::vector<uint8_t>& buf) {
    static std::mt19937 gen(std::random_device{}());
    static std::uniform_int_distribution<> dis(0, 255);
    for (auto& b : buf) b = static_cast<uint8_t>(dis(gen));
}

// 工具：进度条打印 (修复版)
// 逻辑：每当进度增加 5% 或者刚开始/结束时打印
void print_progress(size_t current, size_t total, const std::string& prefix) {
    static int last_percent = -1;
    // 如果是新任务（current=0 或很小），重置状态
    if (current == 0 || current < CHUNK_SIZE) last_percent = -1;

    int percent = (int)((double)current / total * 100);

    // 只有当百分比变化且跨越 5% 阈值，或者完成时才打印
    if (percent != last_percent && (percent % 5 == 0 || current >= total)) {
        // 计算 MB
        size_t current_mb = current >> 20;
        size_t total_mb = total >> 20;

        std::cout << "\r" << prefix << " ["
                  << std::setw(3) << percent << "%] "
                  << current_mb << "MB / " << total_mb << "MB   " << std::flush;
        last_percent = percent;
    }
}

class StressTester {
    std::shared_ptr<FileSys> fs;
    std::mt19937 rng;

public:
    StressTester(std::shared_ptr<FileSys> _fs) : fs(_fs), rng(12345) {}

    // 测试 1: 目录线性扩展能力 (10,000 个文件)
    void test_massive_directory() {
        std::cout << "\n\n[Test 1] 海量目录项测试 (10,000 files)..." << std::endl;
        std::string dir = "/massive_dir";
        fs->create_dir("/", "massive_dir");

        int count = 10000;
        for (int i = 0; i < count; ++i) {
            std::string name = "file_" + std::to_string(i);
            if (!fs->create_file(dir, name)) {
                std::cerr << "\n[Error] 创建文件失败: " << name << std::endl;
                exit(1);
            }
            print_progress(i + 1, count, "Creating");
        }
        std::cout << "\n-> 验证查找..." << std::endl;
        assert(fs->has_file(dir + "/file_0"));
        assert(fs->has_file(dir + "/file_5000"));
        assert(fs->has_file(dir + "/file_9999"));
        std::cout << "-> 验证通过。" << std::endl;
    }

    // 测试 2: GB 级大文件读写 (B-Tree 压力测试)
    void test_gigantic_file() {
        std::cout << "\n[Test 2] 1GB 大文件分块读写测试..." << std::endl;
        std::string path = "/big_file.bin";
        fs->create_file("/", "big_file.bin");

        auto fd_opt = fs->open(path);
        if (!fd_opt) { std::cerr << "无法打开文件" << std::endl; exit(1); }
        uint64_t fd = fd_opt.value();

        size_t total_size = 1024ULL * 1024 * 1024; // 1GB
        size_t written = 0;
        std::vector<uint8_t> buf(CHUNK_SIZE); // 1MB buffer

        // 写入阶段
        std::cout << "-> 正在写入 1GB 数据 (Chunk: 1MB)..." << std::endl;
        while (written < total_size) {
            // 用 pattern 填充数据，方便校验: 每个块首字节存块序号
            std::memset(buf.data(), (written / CHUNK_SIZE) % 255, buf.size());

            if (!fs->write(fd, buf)) {
                std::cerr << "\n[Error] 写入失败 at " << written << std::endl;
                exit(1);
            }
            written += buf.size();
            print_progress(written, total_size, "Writing");
        }
        fs->close(fd);

        // 读取校验阶段
        std::cout << "\n-> 正在回读校验..." << std::endl;
        fd = fs->open(path).value();
        size_t read_total = 0;

        while (read_total < total_size) {
            size_t n = fs->read(fd, buf);
            if (n == 0) break;

            uint8_t expected_val = (read_total / CHUNK_SIZE) % 255;
            if (buf[0] != expected_val) {
                std::cerr << "\n[Error] 数据校验失败 at " << read_total
                          << " Expected: " << (int)expected_val
                          << " Got: " << (int)buf[0] << std::endl;
                exit(1);
            }
            read_total += n;
            print_progress(read_total, total_size, "Reading");
        }
        std::cout << "\n-> 1GB 文件读写校验完成。" << std::endl;
    }

    // 测试 3: 深度递归与路径解析
    void test_deep_recursion() {
        std::cout << "\n[Test 3] 深度递归目录测试 (Depth 100)..." << std::endl;
        std::string current_path = "/";
        int depth = 100;

        for (int i = 0; i < depth; ++i) {
            std::string dirname = "d" + std::to_string(i);
            if (!fs->create_dir(current_path, dirname)) {
                std::cerr << "\n[Error] 目录创建失败 at depth " << i << std::endl;
                exit(1);
            }
            if (current_path == "/") current_path += dirname;
            else current_path += "/" + dirname;
            print_progress(i + 1, depth, "DeepMkdir");
        }

        std::string deep_file = current_path + "/treasure.txt";
        fs->create_file(current_path, "treasure.txt");
        assert(fs->has_file(deep_file));
        std::cout << "\n-> 深度 " << depth << " 路径解析正常。" << std::endl;
    }

    // 测试 4: 长时随机操作 (模拟真实负载)
    void test_random_chaos() {
        std::cout << "\n[Test 4] 随机混乱测试 (10,000 ops)..." << std::endl;
        fs->create_dir("/", "chaos");
        std::vector<std::string> files;

        int ops_count = 10000;
        std::uniform_int_distribution<> op_dist(0, 3); // 0:Create, 1:Write, 2:Read, 3:Delete
        std::uniform_int_distribution<> size_dist(10, 4096 * 2); // 随机写入大小

        for (int i = 0; i < ops_count; ++i) {
            int op = op_dist(rng);

            if (op == 0) { // Create
                std::string fname = "f" + std::to_string(i);
                if (fs->create_file("/chaos", fname)) {
                    files.push_back(fname);
                }
            }
            else if (op == 1 && !files.empty()) { // Write
                std::string fname = files[rng() % files.size()];
                auto fd = fs->open("/chaos/" + fname);
                if (fd) {
                    std::vector<uint8_t> data(size_dist(rng));
                    fill_random(data);
                    fs->write(fd.value(), data);
                    fs->close(fd.value());
                }
            }
            else if (op == 2 && !files.empty()) { // Read
                std::string fname = files[rng() % files.size()];
                auto fd = fs->open("/chaos/" + fname);
                if (fd) {
                    std::vector<uint8_t> buf(1024);
                    fs->read(fd.value(), buf);
                    fs->close(fd.value());
                }
            }
            else if (op == 3 && !files.empty()) { // Delete
                int idx = rng() % files.size();
                std::string fname = files[idx];
                // 简单起见，从列表移除，不管删除是否成功(可能正打开)
                fs->remove_file("/chaos/" + fname);

                // Swap remove from vector
                files[idx] = files.back();
                files.pop_back();
            }
            print_progress(i + 1, ops_count, "ChaosOps");
        }
        std::cout << "\n-> 混乱测试完成，系统未崩溃。" << std::endl;
    }
};

int main() {
    // 1. 初始化日志
    init_logger();
    spdlog::debug("{}", std::string(30, '=') + " Program Start " + std::string(30, '='));

    // 2. 压力测试期间，强制调整日志级别为 WARN
    // 否则大量 I/O 操作产生的 Trace/Debug 日志会把硬盘写满并极大地拖慢速度
    spdlog::set_level(spdlog::level::warn);

    std::cout << "============================================" << std::endl;
    std::cout << "      FILE SYSTEM ULTRA STRESS TEST         " << std::endl;
    std::cout << "      Disk Size: " << DISK_SIZE_GB << " GB" << std::endl;
    std::cout << "============================================" << std::endl;

    // 3. 初始化硬盘和文件系统
    IDisk* disk = new FileDisk(DISK_SIZE_GB, BLOCK_SIZE, DISK_PATH);
    auto fs = std::make_shared<FileSys>(disk);

    // 4. 格式化以确保环境纯净
    // 注意：如果是 4TB 硬盘，format 过程中的 bitmap 初始化可能需要几秒钟
    std::cout << "正在格式化 " << DISK_SIZE_GB << " GB 磁盘..." << std::endl;
    fs->format();

    // 5. 开始测试
    StressTester tester(fs);
    auto start = std::chrono::high_resolution_clock::now();

    tester.test_massive_directory();
    tester.test_gigantic_file();
    tester.test_deep_recursion();
    tester.test_random_chaos();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "\n\n============================================" << std::endl;
    std::cout << "   ALL TESTS PASSED SUCCESSFULLY!            " << std::endl;
    std::cout << "   Total Time: " << std::fixed << std::setprecision(2) << elapsed.count() << "s" << std::endl;
    std::cout << "============================================" << std::endl;

    return 0;
}
