// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace wasurene_update {
namespace fs = std::filesystem;
using json = nlohmann::json;
struct Release { std::string tag, url, digest; uint64_t size = 0; };
struct Layout { std::string plugin; fs::path dll, data; };
struct File { fs::path source, target; };
bool newer(const std::string &candidate, const std::string &current);
Release release_info(const json &data, const std::string &repo, const std::string &asset);
struct Network { std::atomic<bool> cancelled{false}; std::mutex mutex; void *request=nullptr; void cancel(); };
std::vector<unsigned char> download(const std::string &url, size_t limit, Network *network=nullptr);
std::string sha256(const std::vector<unsigned char> &data);
std::vector<unsigned char> read_bytes(const fs::path &path, size_t limit);
void write_bytes(const fs::path &path, const std::vector<unsigned char> &data);
Layout layout(const std::string &plugin, const fs::path &dll, const fs::path &data);
void no_reparse(const fs::path &path);
std::vector<File> unpack(const fs::path &zip, const fs::path &stage, const Layout &layout);
void apply_files(const std::vector<File> &files, const fs::path &backup, int fail_after = -1);
std::wstring wide(const std::string &value);
std::wstring quote(const std::wstring &value);
fs::path executable();
void start_process(const fs::path &exe, const std::wstring &args, const fs::path &cwd);
}
