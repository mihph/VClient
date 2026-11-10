#pragma once
#include <string>
#include <vector>
#include <cstdint>

class DataWriter {
private:
    std::string filePath;
public:
    DataWriter(const std::string& path) : filePath(path) {}
    void saveResults(const std::vector<int32_t>& results);
}; 