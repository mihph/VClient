#pragma once
#include <string>
#include <utility>

class ConfigManager {
private:
    std::string configPath;
public:
    ConfigManager(const std::string& path) : configPath(path) {}
    std::pair<std::string, std::string> loadCredentials();
};