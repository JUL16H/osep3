#pragma once
#include "FileSys.hpp"
#include <filesystem>
#include <format>
#include <iostream>
#include <sstream>
#include <vector>

inline std::string path_join(std::string path1, std::string path2) {
    std::filesystem::path p1(path1);
    std::filesystem::path p2(path2);
    auto joined = p1 / p2;
    return joined.lexically_normal().generic_string();
}

inline std::optional<uint64_t> str2unum(std::string_view str) {
    uint64_t rst = 0;
    for (const char c : str) {
        if (c < '0' || c > '9')
            return std::nullopt;
        rst = 10 * rst + c - '0';
    }
    return rst;
}

class CLI {
public:
    CLI(std::shared_ptr<FileSys> _filesys) : filesys(_filesys) {}

    void run() {
        std::string line;
        std::cout << "Type 'help' to see available commands.\n";

        while (true) {
            std::cout << std::format("{} >", cur_path);
            std::getline(std::cin, line);

            std::stringstream ss(line);
            std::string word;
            std::vector<std::string> args;

            while (ss >> word)
                args.push_back(word);
            if (args.empty())
                continue;
            auto cmd = args[0];

            std::string original_cmd = cmd;
            args[0] = cur_path;

            if (original_cmd == "help") {
                print_help();
            } else if (original_cmd == "ls") {
                switch (args.size()) {
                case 1:
                    filesys->list_directory(args[0]);
                    break;
                case 2:
                    filesys->list_directory(path_join(args[0], args[1]));
                    break;
                }
            } else if (original_cmd == "df") {
                filesys->get_disk_info();
            } else if (original_cmd == "exit") {
                return;
            } else if (original_cmd == "mkdir") {
                if (args.size() != 2)
                    continue;
                filesys->create_dir(path_join(args[0], args[1]));
            } else if (original_cmd == "touch") {
                if (args.size() != 2)
                    continue;
                filesys->create_file(path_join(args[0], args[1]));
            } else if (original_cmd == "rm") {
                if (args.size() != 2) {
                    std::cout << "Usage: rm <filename>\n";
                    continue;
                }
                std::string path = path_join(args[0], args[1]);
                if (filesys->remove_file(path)) {
                    std::cout << "File removed: " << args[1] << "\n";
                } else {
                    std::cout << "Failed to remove file: " << args[1] << "\n";
                }
            } else if (original_cmd == "rmdir") {
                if (args.size() != 2) {
                    std::cout << "Usage: rmdir <dirname>\n";
                    continue;
                }
                std::string path = path_join(args[0], args[1]);
                if (filesys->remove_dir(path)) {
                    std::cout << "Directory removed: " << args[1] << "\n";
                } else {
                    std::cout << "Failed to remove directory: " << args[1]
                              << " (Directory might not be empty)\n";
                }
            } else if (original_cmd == "cd") {
                switch (args.size()) {
                case 1:
                    cur_path = "/";
                    break;
                case 2:
                    std::string new_path = path_join(args[0], args[1]);
                    if (filesys->has_dir(new_path))
                        cur_path = new_path;
                    else
                        std::cout << "Directory not found: " << new_path << "\n";
                    break;
                }
            } else if (original_cmd == "format") {
                std::cout << "执行格式化 [Y/N]:";
                std::string check;
                std::cin >> check;
                std::string dummy;
                std::getline(std::cin, dummy);

                if (check == "Y" || check == "y") {
                    cur_path = "/";
                    filesys->format();
                }
            } else if (original_cmd == "open") {
                if (args.size() < 2) {
                    std::cout << "Usage: open <filename> [offset]\n";
                    continue;
                }
                std::string path = path_join(args[0], args[1]);
                if (!filesys->has_file(path)) {
                    std::cout << "File not found: " << path << "\n";
                    continue;
                }
                uint64_t offset = 0;
                if (args.size() == 3) {
                    auto off_opt = str2unum(args[2]);
                    if (off_opt)
                        offset = off_opt.value();
                }
                auto fd = filesys->open(path, offset);
                if (fd)
                    std::cout << "File Descriptor: " << fd.value() << "\n";
                else
                    std::cout << "Failed to open file.\n";

            } else if (original_cmd == "close") {
                if (args.size() != 2) {
                    std::cout << "Usage: close <fd>\n";
                    continue;
                }
                auto fd_opt = str2unum(args[1]);
                if (fd_opt)
                    filesys->close(fd_opt.value());

            } else if (original_cmd == "seek") {
                if (args.size() != 3) {
                    std::cout << "Usage: seek <fd> <offset>\n";
                    continue;
                }
                auto fd_opt = str2unum(args[1]);
                auto off_opt = str2unum(args[2]);

                if (!fd_opt || !off_opt) {
                    std::cout << "Invalid arguments.\n";
                    continue;
                }

                filesys->seek(fd_opt.value(), off_opt.value());
                std::cout << "Seeked FD " << fd_opt.value() << " to offset " << off_opt.value()
                          << "\n";

            } else if (original_cmd == "write") {
                if (args.size() != 3) {
                    std::cout << "Usage: write <fd> <content_string>\n";
                    continue;
                }
                auto fd_opt = str2unum(args[1]);
                if (!fd_opt) {
                    std::cout << "Invalid FD.\n";
                    continue;
                }
                filesys->write(fd_opt.value(),
                               std::span<uint8_t>(reinterpret_cast<uint8_t *>(args[2].data()),
                                                  args[2].size()));
                std::cout << "Written " << args[2].size() << " bytes.\n";

            } else if (original_cmd == "read") {
                if (args.size() != 3) {
                    std::cout << "Usage: read <fd> <size>\n";
                    continue;
                }
                auto fd_opt = str2unum(args[1]);
                auto size_opt = str2unum(args[2]);

                if (!fd_opt || !size_opt) {
                    std::cout << "Invalid arguments.\n";
                    continue;
                }

                uint64_t fd = fd_opt.value();
                uint64_t size = size_opt.value();
                std::vector<uint8_t> buffer(size);

                size_t read_cnt = filesys->read(fd, buffer);
                if (read_cnt > 0) {
                    for (uint64_t i = 0; i < buffer.size(); i++)
                        buffer[i] = buffer[i] == 0 ? '.' : buffer[i];
                    std::string out(buffer.begin(), buffer.begin() + read_cnt);
                    std::cout << out << "\n";
                } else {
                    std::cout << "(Empty or EOF)\n";
                }

            } else if (original_cmd == "cat") {
                if (args.size() < 2) {
                    std::cout << "Usage: cat <filename>\n";
                    continue;
                }
                std::string path = path_join(args[0], args[1]);
                if (!filesys->has_file(path)) {
                    std::cout << "File not found: " << path << "\n";
                    continue;
                }

                auto fd_opt = filesys->open(path, 0);
                if (!fd_opt) {
                    std::cout << "Failed to open file.\n";
                    continue;
                }
                uint64_t fd = fd_opt.value();

                const size_t buf_size = 1024;
                std::vector<uint8_t> buffer(buf_size);
                while (true) {
                    size_t n = filesys->read(fd, buffer);
                    if (n == 0)
                        break;
                    for (size_t i = 0; i < n; ++i) {
                        if (buffer[i] == 0) {
                            buffer[i] = '.';
                        }
                    }
                    std::cout.write(reinterpret_cast<char *>(buffer.data()), n);
                }
                std::cout << "\n";
                filesys->close(fd);

            } else if (original_cmd == "mkdirn") {
                if (args.size() != 3) {
                    std::cout << "Usage: mkdirn <name_prefix> <count>\n";
                    continue;
                }
                auto n_opt = str2unum(args[2]);
                if (!n_opt) {
                    std::cout << "Invalid number: " << args[2] << "\n";
                    continue;
                }

                std::string prefix = args[1];
                uint64_t n = n_opt.value();
                int success_count = 0;

                for (uint64_t i = 0; i < n; ++i) {
                    std::string name = prefix + std::to_string(i);
                    if (filesys->create_dir(args[0] + "/" + name)) {
                        success_count++;
                    } else {
                        std::cout << "Failed to create directory: " << name << "\n";
                    }
                }
                std::cout << "Batch created " << success_count << " directories.\n";

            } else if (original_cmd == "touchn") {
                if (args.size() != 3) {
                    std::cout << "Usage: touchn <name_prefix> <count>\n";
                    continue;
                }
                auto n_opt = str2unum(args[2]);
                if (!n_opt) {
                    std::cout << "Invalid number: " << args[2] << "\n";
                    continue;
                }

                std::string prefix = args[1];
                uint64_t n = n_opt.value();
                int success_count = 0;

                for (uint64_t i = 0; i < n; ++i) {
                    std::string name = prefix + std::to_string(i);
                    if (filesys->create_file(args[0] + "/" + name)) {
                        success_count++;
                    } else {
                        std::cout << "Failed to create file: " << name << "\n";
                    }
                }
                std::cout << "Batch created " << success_count << " files.\n";

            } else {
                std::cout << "Unknown command: " << original_cmd << "\n";
            }
        }
    }

private:
    void print_help() {
        std::cout << "Available commands:\n";
        std::cout << "  ls [path]               List directory contents\n";
        std::cout << "  cd <path>               Change directory\n";
        std::cout << "  mkdir <name>            Create directory\n";
        std::cout << "  touch <name>            Create file\n";
        std::cout << "  rm <name>               Remove file\n";
        std::cout << "  rmdir <name>            Remove directory (must be empty)\n";
        std::cout << "  cat <name>              Display file content\n";
        std::cout << "  open <name> [offset]    Open file\n";
        std::cout << "  close <fd>              Close file\n";
        std::cout << "  read <fd> <size>        Read from file descriptor\n";
        std::cout << "  write <fd> <content>    Write to file descriptor\n";
        std::cout << "  seek <fd> <offset>      Seek to offset in file\n";
        std::cout << "  format                  Format file system\n";
        std::cout << "  mkdirn <prefix> <n>     Batch create directories\n";
        std::cout << "  touchn <prefix> <n>     Batch create files\n";
        std::cout << "  exit                    Exit the system\n";
        std::cout << "  help                    Show this help message\n";
    }

private:
    std::string cur_path = "/";
    std::shared_ptr<FileSys> filesys;
};
