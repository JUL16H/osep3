#include "FileSys.hpp"

int main() {
    spdlog::set_level(spdlog::level::debug);

    VDisk disk(4096, "vdisk.img");
    FileSys sys(disk);

    return 0;
}
