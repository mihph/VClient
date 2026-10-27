#include "writer.h"
#include <fstream>
#include <stdexcept>

void DataWriter::saveResults(const std::vector<int32_t>& results) {
    std::ofstream file(filePath);
    if (!file.is_open()) {
        throw std::runtime_error("Ошибка открытия выходного файла: " + filePath);
    }
    
    file << results.size();
    for (int32_t res : results) {
        file << " " << res;
    }
    file << std::endl;
}