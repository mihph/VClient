#include "config.h"
#include <fstream>
#include <stdexcept>

std::pair<std::string, std::string> ConfigManager::loadCredentials() {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        throw std::runtime_error("Не удалось открыть файл конфигурации: " + configPath);
    }
    std::string login, pass;
    if (!(file >> login >> pass)) {
        throw std::runtime_error("Ошибка формата файла конфигурации (ожидается LOGIN и PASSWORD)");
    }
    return {login, pass};
}