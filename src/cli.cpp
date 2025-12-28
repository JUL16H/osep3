#include "FileDisk.hpp"
#include "CLI.hpp"
#include "FileSys.hpp"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <string>
#include <cassert>

void init_logger() {
    auto logger = spdlog::rotating_logger_mt("basic_logger", "log.log", 5ULL << 20, 5);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    spdlog::set_level(spdlog::level::debug);
}

int main() {
    init_logger();

    auto disk = std::make_shared<FileDisk>(4096, BLOCK_SIZE, "vdisk_test.img");
    auto filesys = std::make_shared<FileSys>(disk);

    filesys->format();
    filesys->create_dir("/??");

    CLI cli(filesys);
    cli.run();

    return 0;
}
