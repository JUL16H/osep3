#include "CLI.hpp"
#include "FileDisk.hpp"
#include "FileSys.hpp"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

void init_logger() {
    auto logger = spdlog::rotating_logger_mt("basic_logger", "log.log", 5ULL << 20, 5);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_on(spdlog::level::trace);
}

int main() {
    init_logger();
    spdlog::debug("{}", std::string(30, '=') + " Program Start " + std::string(30, '='));

    IDisk *disk = new FileDisk(4096, BLOCK_SIZE, "vdisk.img");
    auto sys = std::make_shared<FileSys>(disk);

    CLI cli(sys);
    cli.run();

    return 0;
}
