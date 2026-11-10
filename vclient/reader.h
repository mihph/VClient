#pragma once
#include <string>
#include <vector>
#include <cstdint>

class DataReader {
private:
    std::string filePath;
public:
    DataReader(const std::string& path) : filePath(path) {}
    std::vector<std::vector<int32_t>> readVectors();
};