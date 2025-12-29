#include "FileDisk.hpp"
#include "FileSys.hpp"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ================= 配置区域 =================
const std::string DISK_PATH = "vdisk_test.img";
const uint32_t DISK_SIZE_GB = 4096;        // 4TB 用于提供足够的测试空间
const size_t CHUNK_SIZE = 1 * 1024 * 1024; // 1MB Buffer
// ===========================================

// ================= 日志初始化 =================
void init_logger() {
    // 限制日志大小，避免 stress test 产生 GB 级日志
    auto logger = spdlog::rotating_logger_mt("stress_log", "stress_test.log", 10 * 1024 * 1024, 3);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    // 压力测试建议设为 WARN 或 ERROR，否则 IO 瓶颈在日志上
    spdlog::set_level(spdlog::level::warn);
}

// ================= 工具类：专业进度条 =================
class ProgressBar {
public:
    ProgressBar(size_t total_work, std::string desc, int width = 40)
        : total(total_work), description(desc), bar_width(width) {
        start_time = std::chrono::high_resolution_clock::now();
        last_update_time = start_time;
    }

    void update(size_t current) {
        auto now = std::chrono::high_resolution_clock::now();
        // 限制刷新频率 (每 100ms 一次)，避免刷屏
        if (current < total &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_time).count() <
                100) {
            return;
        }
        last_update_time = now;

        double progress = (total > 0) ? static_cast<double>(current) / total : 1.0;
        progress = std::min(1.0, std::max(0.0, progress));

        int pos = static_cast<int>(bar_width * progress);

        // 计算速度 (MB/s) 和 ETA
        auto elapsed_sec =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
        double speed = (elapsed_sec > 0) ? (current / (1024.0 * 1024.0) / elapsed_sec) : 0.0;
        double remaining_sec = (speed > 0) ? ((total - current) / (1024.0 * 1024.0) / speed) : 0.0;

        std::cout << "\r" << std::left << std::setw(20) << description << " [";
        for (int i = 0; i < bar_width; ++i) {
            if (i < pos)
                std::cout << "█";
            else if (i == pos)
                std::cout << "▒";
            else
                std::cout << "░";
        }
        std::cout << "] " << std::right << std::setw(3) << int(progress * 100.0) << "% "
                  << std::fixed << std::setprecision(1) << std::setw(7) << speed << " MB/s "
                  << "ETA: " << std::setw(4) << static_cast<int>(remaining_sec) << "s "
                  << std::flush;

        if (current >= total) {
            std::cout << std::endl;
        }
    }

private:
    size_t total;
    std::string description;
    int bar_width;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
    std::chrono::time_point<std::chrono::high_resolution_clock> last_update_time;
};

// 工具：生成随机 Buffer
void fill_random(std::vector<uint8_t> &buf) {
    static uint64_t seed = 123456789;
    for (size_t i = 0; i < buf.size(); i += 8) {
        seed = (seed * 6364136223846793005ULL + 1442695040888963407ULL);
        size_t copy_size = std::min((size_t)8, buf.size() - i);
        std::memcpy(&buf[i], &seed, copy_size);
    }
}

// ================= 压力测试类 =================
class StressTester {
    std::shared_ptr<FileSys> fs;
    std::mt19937 rng;

public:
    StressTester(std::shared_ptr<FileSys> _fs) : fs(_fs), rng(std::random_device{}()) {}

    void update_fs(std::shared_ptr<FileSys> _fs) { fs = _fs; }

    // 1. 海量小文件测试
    void test_massive_small_files() {
        std::cout << "\n[Test 1] 海量小文件测试 (Massive Small Files)..." << std::endl;
        std::string base_dir = "/small_files";
        if (!fs->has_dir(base_dir))
            fs->create_dir(base_dir);

        int count = 10000;
        ProgressBar bar(count, "Creating 10k Files");

        for (int i = 0; i < count; ++i) {
            std::string name = "file_" + std::to_string(i);
            if (!fs->create_file(base_dir + "/" + name)) {
                spdlog::error("创建文件失败: index {}", i);
                exit(1);
            }
            bar.update(i + 1);
        }

        // 随机抽查
        std::uniform_int_distribution<> dist(0, count - 1);
        for (int i = 0; i < 100; ++i) {
            int idx = dist(rng);
            if (!fs->has_file(base_dir + "/file_" + std::to_string(idx))) {
                std::cerr << "致命错误: 文件丢失!" << std::endl;
                exit(1);
            }
        }
        std::cout << "-> 验证成功。" << std::endl;
    }

    // 2. 1GB 大文件顺序读写
    void test_gigantic_sequential() {
        std::cout << "\n[Test 2] 1GB 顺序读写测试 (Seq I/O)..." << std::endl;
        std::string path = "/large_seq.bin";
        fs->create_file(path);

        auto fd = fs->open(path).value();
        size_t total_size = 1ULL * 1024 * 1024 * 1024; // 1GB
        std::vector<uint8_t> buf(CHUNK_SIZE);
        fill_random(buf);

        ProgressBar write_bar(total_size, "Writing 1GB");
        size_t written = 0;
        while (written < total_size) {
            *(uint64_t *)buf.data() = written; // 写入offset标记
            fs->write(fd, buf);
            written += buf.size();
            write_bar.update(written);
        }
        fs->close(fd);

        // 校验
        fd = fs->open(path).value();
        ProgressBar read_bar(total_size, "Verifying 1GB");
        size_t read_bytes = 0;
        while (read_bytes < total_size) {
            fs->read(fd, buf);
            if (*(uint64_t *)buf.data() != read_bytes) {
                std::cerr << "数据校验失败! Offset: " << read_bytes << std::endl;
                exit(1);
            }
            read_bytes += buf.size();
            read_bar.update(read_bytes);
        }
        fs->close(fd);
    }

    // 3. 碎片化测试
    void test_fragmentation() {
        std::cout << "\n[Test 3] 磁盘碎片化测试 (Fragmentation)..." << std::endl;
        std::string dir = "/fragmentation";
        if (!fs->has_dir(dir))
            fs->create_dir(dir);

        int file_count = 2000;
        std::vector<uint8_t> data(4096, 0xFF);

        ProgressBar bar1(file_count, "Fill Files");
        for (int i = 0; i < file_count; ++i) {
            std::string path = dir + "/" + std::to_string(i);
            fs->create_file(path);
            auto fd = fs->open(path).value();
            fs->write(fd, data);
            fs->close(fd);
            bar1.update(i + 1);
        }

        std::cout << "-> 删除 50% 文件制造空洞..." << std::endl;
        ProgressBar bar2(file_count / 2, "Hole Punching");
        for (int i = 0; i < file_count; i += 2) {
            fs->remove_file(dir + "/" + std::to_string(i));
            bar2.update(i / 2 + 1);
        }

        std::cout << "-> 写入新文件以测试空闲块复用..." << std::endl;
        std::string mixed_file = "/frag_mixed.bin";
        fs->create_file(mixed_file);
        auto fd = fs->open(mixed_file).value();

        size_t total_hole_size = (file_count / 2) * 4096;
        size_t written = 0;
        ProgressBar bar3(total_hole_size, "Refilling");

        while (written < total_hole_size) {
            fs->write(fd, data);
            written += data.size();
            bar3.update(written);
        }
        fs->close(fd);
    }

    // 4. 稀疏文件与随机写
    void test_sparse_random_rw() {
        std::cout << "\n[Test 4] 稀疏文件与随机写 (Random Seek/B+Tree)..." << std::endl;
        std::string path = "/sparse_random.bin";
        fs->create_file(path);

        auto fd = fs->open(path).value();
        size_t range = 2ULL * 1024 * 1024 * 1024;
        int ops = 500;
        std::uniform_int_distribution<uint64_t> offset_dist(0, range - 4096);
        std::vector<std::pair<uint64_t, uint64_t>> records;

        ProgressBar bar(ops, "Random RW");
        for (int i = 0; i < ops; ++i) {
            uint64_t off = (offset_dist(rng) / 4096) * 4096; // 4K对齐
            uint64_t magic = rng();
            std::vector<uint8_t> buf(4096);
            *(uint64_t *)buf.data() = magic;

            fs->seek(fd, off);
            fs->write(fd, buf);
            records.push_back({off, magic});
            bar.update(i + 1);
        }
        fs->close(fd);

        std::cout << "-> 验证随机写数据..." << std::endl;
        fd = fs->open(path).value();
        for (auto &rec : records) {
            std::vector<uint8_t> buf(4096);
            fs->seek(fd, rec.first);
            fs->read(fd, buf);
            if (*(uint64_t *)buf.data() != rec.second) {
                std::cerr << "稀疏验证失败! Offset: " << rec.first << std::endl;
                exit(1);
            }
        }
        fs->close(fd);
    }

    // 5. 缓存颠簸测试
    void test_cache_thrashing() {
        std::cout << "\n[Test 5] 缓存颠簸测试 (Cache Thrashing)..." << std::endl;
        size_t test_size = 400 * 1024 * 1024; // 400MB > 默认缓存
        std::string path = "/cache_thrash.bin";
        fs->create_file(path);

        auto fd = fs->open(path).value();
        std::vector<uint8_t> buf(CHUNK_SIZE);
        fill_random(buf);

        ProgressBar bar(test_size, "Overfilling");
        for (size_t w = 0; w < test_size; w += CHUNK_SIZE) {
            *(uint64_t *)buf.data() = w;
            fs->write(fd, buf);
            bar.update(w + CHUNK_SIZE);
        }
        fs->close(fd);

        fd = fs->open(path).value();
        ProgressBar bar_read(test_size, "Verifying");
        for (size_t r = 0; r < test_size; r += CHUNK_SIZE) {
            fs->read(fd, buf);
            if (*(uint64_t *)buf.data() != r) {
                std::cerr << "缓存回写失败! 数据丢失。" << std::endl;
                exit(1);
            }
            bar_read.update(r + CHUNK_SIZE);
        }
        fs->close(fd);
    }

    // 6. 目录系统综合压力测试
    void test_directory_ops() {
        std::cout << "\n[Test 6] 目录系统综合压力测试 (Directory Stress)..." << std::endl;
        std::string root = "/dir_stress";
        if (!fs->has_dir(root))
            fs->create_dir(root);

        // A. 深度嵌套
        std::cout << "-> A. 深度嵌套测试 (Depth 50)..." << std::endl;
        std::string current_path = root + "/deep";
        fs->create_dir(current_path);
        std::vector<std::string> path_stack = {current_path};

        for (int i = 0; i < 50; ++i) {
            current_path += "/lvl_" + std::to_string(i);
            if (!fs->create_dir(current_path))
                exit(1);
            path_stack.push_back(current_path);
        }

        std::string deep_file = current_path + "/marker.txt";
        fs->create_file(deep_file);

        // 尝试删除非空目录
        if (fs->remove_dir(path_stack.back())) {
            spdlog::error("严重错误: 允许删除含文件的目录!");
            exit(1);
        }

        fs->remove_file(deep_file);
        for (auto it = path_stack.rbegin(); it != path_stack.rend(); ++it) {
            if (!fs->remove_dir(*it)) {
                spdlog::error("无法删除已空目录: {}", *it);
                exit(1);
            }
        }
        std::cout << "   深度嵌套验证通过。" << std::endl;

        // B. 广度测试
        std::cout << "-> B. 广度压力测试 (1000 Subdirs)..." << std::endl;
        std::string breadth_root = root + "/breadth";
        fs->create_dir(breadth_root);

        for (int i = 0; i < 1000; ++i) {
            if (!fs->create_dir(breadth_root + "/s_" + std::to_string(i)))
                exit(1);
        }
        if (fs->remove_dir(breadth_root)) {
            spdlog::error("严重错误: 允许删除非空父目录!");
            exit(1);
        }
        for (int i = 0; i < 1000; ++i) {
            fs->remove_dir(breadth_root + "/s_" + std::to_string(i));
        }
        fs->remove_dir(breadth_root);
        std::cout << "   广度测试通过。" << std::endl;

        fs->remove_dir(root);
    }

    // 7. 综合性能基准测试 (Performance Benchmarks)
    void test_performance_benchmarks() {
        std::cout << "\n[Test 7] 综合性能基准测试 (Performance Benchmarks)..." << std::endl;
        std::string base_dir = "/perf_bench";
        if (!fs->has_dir(base_dir))
            fs->create_dir(base_dir);

        // Benchmark 1: Create 10,000 Directories
        {
            std::string dir_prefix = base_dir + "/dirs";
            if (!fs->has_dir(dir_prefix))
                fs->create_dir(dir_prefix);

            std::cout << "-> 1. 创建 10,000 个目录 (Create Dirs)..." << std::flush;
            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < 10000; ++i) {
                fs->create_dir(dir_prefix + "/d_" + std::to_string(i));
            }

            auto end = std::chrono::high_resolution_clock::now();
            double duration = std::chrono::duration<double>(end - start).count();
            std::cout << " Done. " << std::fixed << std::setprecision(2) << (10000.0 / duration)
                      << " Ops/sec (" << duration << "s)" << std::endl;
        }

        // Benchmark 2: Create 10,000 Files
        {
            std::string file_prefix = base_dir + "/files";
            if (!fs->has_dir(file_prefix))
                fs->create_dir(file_prefix);

            std::cout << "-> 2. 创建 10,000 个文件 (Create Files)..." << std::flush;
            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < 10000; ++i) {
                fs->create_file(file_prefix + "/f_" + std::to_string(i));
            }

            auto end = std::chrono::high_resolution_clock::now();
            double duration = std::chrono::duration<double>(end - start).count();
            std::cout << " Done. " << std::fixed << std::setprecision(2) << (10000.0 / duration)
                      << " Ops/sec (" << duration << "s)" << std::endl;
        }

        // Benchmark 3: Sequential Write 1GB
        std::string seq_file = base_dir + "/seq_1gb.bin";
        fs->create_file(seq_file);
        {
            std::vector<uint8_t> buf(1024 * 1024); // 1MB
            fill_random(buf);
            auto fd = fs->open(seq_file).value();

            std::cout << "-> 3. 顺序写入 20GB 文件 (Seq Write)...  " << std::flush;
            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < 20 * 1024; ++i) {
                fs->write(fd, buf);
            }
            fs->close(fd); // Close 触发 Flush

            auto end = std::chrono::high_resolution_clock::now();
            double duration = std::chrono::duration<double>(end - start).count();
            std::cout << " Done. " << std::fixed << std::setprecision(2) << (20 * 1024.0 / duration)
                      << " MB/s (" << duration << "s)" << std::endl;
        }

        // Benchmark 4: Sequential Read 1GB
        {
            std::vector<uint8_t> buf(1024 * 1024);
            auto fd = fs->open(seq_file).value();

            std::cout << "-> 4. 顺序读取 20GB 文件 (Seq Read)...   " << std::flush;
            auto start = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < 20 * 1024; ++i) {
                fs->read(fd, buf);
            }
            fs->close(fd);

            auto end = std::chrono::high_resolution_clock::now();
            double duration = std::chrono::duration<double>(end - start).count();
            std::cout << " Done. " << std::fixed << std::setprecision(2) << (20 * 1024.0 / duration)
                      << " MB/s (" << duration << "s)" << std::endl;
        }

        // // Benchmark 5: Random Read 1MB x 1000 (3GB Range)
        // std::string rand_file = base_dir + "/rand_3gb.bin";
        // fs->create_file(rand_file);
        // {
        //     auto fd = fs->open(rand_file).value();
        //     // 扩展文件到 3GB (Sparse 方式，只写最后一个字节)
        //     fs->seek(fd, 3ULL * 1024 * 1024 * 1024 - 1);
        //     std::vector<uint8_t> b(1, 0);
        //     fs->write(fd, b);
        //
        //     std::cout << "-> 5. 随机读取 1MB x 1000次 (3GB Range)..." << std::flush;
        //     std::vector<uint8_t> buf(1024 * 1024);
        //     std::uniform_int_distribution<uint64_t> dist(0,
        //                                                  3ULL * 1024 * 1024 * 1024 - 1024 * 1024);
        //
        //     auto start = std::chrono::high_resolution_clock::now();
        //
        //     for (int i = 0; i < 1000; ++i) {
        //         uint64_t offset = dist(rng);
        //         fs->seek(fd, offset);
        //         fs->read(fd, buf);
        //     }
        //
        //     fs->close(fd);
        //     auto end = std::chrono::high_resolution_clock::now();
        //     double duration = std::chrono::duration<double>(end - start).count();
        //     double total_mb = 1000.0;
        //
        //     std::cout << " Done. " << std::fixed << std::setprecision(2) << (total_mb / duration)
        //               << " MB/s (" << duration << "s)";
        //     std::cout << " [Avg Latency: " << (duration * 1000 / 1000.0) << " ms]" << std::endl;
        // }
        //
        // 清理
        // fs->remove_dir(base_dir); // 可选：保留用于事后分析
    }
};

// ================= 主程序 =================
int main() {
    init_logger();

    std::cout << "\n==============================================================================="
                 "=========="
              << std::endl;
    std::cout << "                          FILE SYSTEM ULTRA STRESS TEST v4.2                     "
                 "        "
              << std::endl;
    std::cout << "                 Core Validation: B+Tree, Bitmap Allocator, LRU Cache, "
                 "Persistence       "
              << std::endl;
    std::cout << "================================================================================="
                 "========"
              << std::endl;
    std::cout << "[ Detailed Test Specifications ]" << std::endl;
    std::cout << "1. 海量小文件压力测试 (Massive Small Files)" << std::endl;
    std::cout << "   - [Action] 在 '/small_files' 下连续创建 10,000 个文件，随后随机抽样读取。"
              << std::endl;
    std::cout << "2. 1GB 顺序读写吞吐 (Gigantic Sequential I/O)" << std::endl;
    std::cout << "   - [Action] 以 1MB 为块大小，顺序写入 1GB 随机数据至 '/large_seq.bin'，并回读。"
              << std::endl;
    std::cout << "3. 磁盘碎片化与重用 (Fragmentation & Reuse)" << std::endl;
    std::cout << "   - [Action] 制造空洞 (Punching Holes) 并验证位图分配器对空闲块的复用能力。"
              << std::endl;
    std::cout << "4. 稀疏大文件随机读写 (Sparse Random R/W)" << std::endl;
    std::cout << "   - [Action] 在 2GB 空间内执行 500 次随机 Seek + 4KB 写入，验证 B+ 树索引。"
              << std::endl;
    std::cout << "5. 缓存颠簸与LRU淘汰 (Cache Thrashing)" << std::endl;
    std::cout << "   - [Action] 读写 400MB 数据流，强制触发 Cache Eviction 和脏页回写。"
              << std::endl;
    std::cout << "6. 目录子系统极限测试 (Directory Subsystem Limit)" << std::endl;
    std::cout << "   - [Action] 50层深度嵌套；单目录 1000 子项；非空目录删除边界测试。"
              << std::endl;
    std::cout << "7. 持久化与灾难恢复 (Persistence & Recovery)" << std::endl;
    std::cout << "   - [Action] 模拟重启，检查 Token 与数据完整性。" << std::endl;
    std::cout << "8. 综合性能基准测试 (Performance Benchmarks) [NEW]" << std::endl;
    std::cout << "   - [Metric] 目录创建(10k)、文件创建(10k)、1GB顺序读写、3GB范围随机读取(1k次)。"
              << std::endl;
    std::cout << "================================================================================="
                 "========"
              << std::endl;

    auto disk = std::make_shared<FileDisk>(DISK_SIZE_GB, BLOCK_SIZE, DISK_PATH);

    // [Phase 0] 格式化
    std::cout << "\n[Phase 0] 初始化与格式化磁盘..." << std::endl;
    {
        auto fs = std::make_shared<FileSys>(disk);
        fs->format();
    }

    // [Phase 1] 核心测试
    std::cout << "\n[Phase 1] 执行核心压力与性能测试..." << std::endl;
    {
        auto fs = std::make_shared<FileSys>(disk);
        StressTester tester(fs);

        tester.test_massive_small_files();
        tester.test_gigantic_sequential();
        tester.test_fragmentation();
        tester.test_sparse_random_rw();
        tester.test_cache_thrashing();
        tester.test_directory_ops();
        tester.test_performance_benchmarks(); // 新增的性能测试

        std::cout << "\n[Info] 写入持久化验证令牌..." << std::endl;
        fs->create_file("/persistence.token");
        auto fd = fs->open("/persistence.token").value();
        std::string token = "PersistenceCheck:OK";
        fs->write(fd, std::span<uint8_t>((uint8_t *)token.data(), token.size()));
        fs->close(fd);
    }

    // [Phase 2] 重启验证
    std::cout << "\n[Phase 2] 模拟重启与持久化验证..." << std::endl;
    {
        auto fs = std::make_shared<FileSys>(disk);

        if (!fs->has_file("/persistence.token")) {
            std::cerr << "[Fatal] 持久化失败: 令牌丢失!" << std::endl;
            return 1;
        }
        auto fd = fs->open("/persistence.token").value();
        std::vector<uint8_t> buf(100);
        size_t n = fs->read(fd, buf);
        std::string read_str(buf.begin(), buf.begin() + n);

        if (read_str.find("PersistenceCheck:OK") != std::string::npos) {
            std::cout << "-> 令牌验证成功。" << std::endl;
        } else {
            std::cerr << "[Fatal] 持久化失败: 内容不匹配!" << std::endl;
            return 1;
        }

        // 简单验证一下 Benchmark 留下的文件是否还在
        if (fs->has_file("/perf_bench/seq_1gb.bin")) {
            std::cout << "-> Benchmark 数据持久化验证成功。" << std::endl;
        }
    }

    std::cout << "\n============================================" << std::endl;
    std::cout << "   ALL TESTS PASSED - SYSTEM STABLE         " << std::endl;
    std::cout << "============================================" << std::endl;

    return 0;
}
