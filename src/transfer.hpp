#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ft {
int run(const std::vector<std::string>& args, bool sending);

// Windows uses wmain so non-ASCII file names survive command-line decoding.
template <typename Char>
std::vector<std::string> arguments(int argc, Char** argv) {
    std::vector<std::string> result;
    for (int i = 1; i < argc; ++i) {
        if constexpr (sizeof(Char) == sizeof(char)) result.emplace_back(argv[i]);
        else result.push_back(std::filesystem::path(argv[i]).u8string());
    }
    return result;
}
} // namespace ft
