// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <memory>
#include <string>
namespace wasurene_input {
class DisplayService {
public:
    DisplayService(std::filesystem::path html, std::filesystem::path config, int port=17861);
    ~DisplayService();
    bool start(std::string &error);
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
