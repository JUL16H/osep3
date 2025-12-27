#pragma once
#include "FileSys.hpp"
#include <filesystem>
#include <format>
#include <sstream>

inline std::string path_join(std::string path1, std::string path2) {
    std::filesystem::path p1(path1);
    std::filesystem::path p2(path2);
    auto joined = p1 / p2;
    return joined.lexically_normal().generic_string();
}

class CLI {
public:
    CLI(std::shared_ptr<FileSys> _filesys) : filesys(_filesys) {}

    void run() {
        std::string line;
        while (true) {
            std::cout << std::format("{} >", cur_path);
            std::getline(std::cin, line);
            std::cout << "\n";

            std::stringstream ss(line);
            std::string word;
            std::vector<std::string> args;

            while (ss >> word)
                args.push_back(word);
            if (args.empty())
                continue;
            auto cmd = args[0];
            args[0] = cur_path;

            if (cmd == "ls") {
                switch (args.size()) {
                case 1:
                    filesys->list_directory(args[0]);
                    break;
                case 2:
                    filesys->list_directory(path_join(args[0], args[1]));
                    break;
                }
            } else if (cmd == "exit") {
                return;
            } else if (cmd == "mkdir") {
                switch (args.size()) {
                case 2:
                    // path name
                    filesys->create_dir(args[0], args[1]);
                    break;
                case 3:
                    // cur_path, path, name
                    filesys->create_dir(path_join(args[0], args[1]), args[2]);
                    break;
                }
            } else if (cmd == "touch") {
                switch (args.size()) {
                case 2:
                    // path name
                    filesys->create_file(args[0], args[1]);
                    break;
                case 3:
                    // cur_path, path, name
                    filesys->create_file(path_join(args[0], args[1]), args[2]);
                    break;
                }
            } else if (cmd == "cd") {
                switch (args.size()) {
                case 1:
                    cur_path = "/";
                    break;
                case 2:
                    std::string new_path = path_join(args[0], args[1]);
                    if (filesys->has_dir(new_path))
                        cur_path = new_path;
                    break;
                }
            } else if (cmd == "format") {
                std::cout << "执行格式化 [Y/N]:";
                std::string check;
                std::cin >> check;
                if (check == "Y" || check == "y") {
                    cur_path = "/";
                    filesys->format();
                }
                std::getline(std::cin, check);
            }
        }
    }

private:
    std::string cur_path = "/";
    std::shared_ptr<FileSys> filesys;
};
