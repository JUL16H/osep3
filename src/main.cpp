#include "FileSys.hpp"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

void init_logger() {
    auto logger = spdlog::rotating_logger_mt("basic_logger", "log.log", 5ULL << 20, 5);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_level(spdlog::level::trace);
}

int main() {
    init_logger();
    spdlog::debug("{}", std::string(30, '=') + " Program Start " + std::string(30, '='));

    VDisk disk(4096, BLOCK_SIZE, "vdisk.img");
    FileSys sys(disk);

    sys.format();
    for (int i = 0; i < 100; i++)
        sys.create_dir(0, "FileSys" + std::to_string(i));
    sys.list_directory(0);

    return 0;
}
