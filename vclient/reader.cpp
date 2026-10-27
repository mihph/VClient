#include "reader.h"
#include <fstream>
#include <stdexcept>

std::vector<std::vector<int32_t>> DataReader::readVectors() {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        throw std::runtime_error("Ошибка открытия входного файла: " + filePath);
    }

    uint32_t count;
    if (!(file >> count)) {
        throw std::runtime_error("Ошибка чтения количества векторов в файле: " + filePath);
    }

    std::vector<std::vector<int32_t>> data(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size;
        if (!(file >> size)) {
            throw std::runtime_error("Ошибка чтения размера вектора #" + std::to_string(i));
        }
        data[i].resize(size);
        for (uint32_t j = 0; j < size; ++j) {
            if (!(file >> data[i][j])) {
                throw std::runtime_error("Ошибка чтения значения вектора #" + std::to_string(i) + ", элемент " + std::to_string(j));
            }
        }
    }
    return data;
} 