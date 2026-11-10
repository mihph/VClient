#include "client.h"
#include "config.h"
#include "reader.h"
#include "writer.h"
#include "communicator.h"
#include "auth.h"
#include <iostream>
#include <stdexcept>

void Client::handleError(const std::string& func, const std::string& context, const std::exception& e) {
    std::cerr << "=== ОШИБКА ===" << std::endl;
    std::cerr << "Функция/Модуль: " << func << std::endl;
    std::cerr << "Контекст/Параметры: " << context << std::endl;
    std::cerr << "Тип ошибки: " << e.what() << std::endl;
    std::cerr << "===============" << std::endl;
}

int Client::start(int argc, char** argv) {
    (void)argc; (void)argv;
    try {
        run();
        return 0;
    } catch (const std::exception& e) {
        handleError("Client::run", "Общая ошибка выполнения", e);
        return 1;
    }
}

void Client::run() {
    ConfigManager config(params.configFile);
    std::pair<std::string, std::string> creds = config.loadCredentials();
    std::string login = creds.first;
    std::string pass = creds.second;

    DataReader reader(params.inputFile);
    std::vector<std::vector<int32_t>> vectors = reader.readVectors();

    Communicator comm;
    comm.connectServer(params.serverIp, params.serverPort);
    
    comm.sendLogin(login);
    std::string salt = comm.receiveSalt();
    
    if (salt == "ERR") {
        throw std::runtime_error("Сервер отклонил LOGIN (неверный идентификатор)");
    }

    std::string hash = Authenticator::computeSHA256(salt, pass);
    comm.sendHash(hash);
    
    std::string status = comm.receiveAuthStatus();
    if (status == "ERR") {
        throw std::runtime_error("Ошибка аутентификации (неверный хэш пароля)");
    }
    if (status != "OK") {
        throw std::runtime_error("Неожиданный ответ сервера при аутентификации: " + status);
    }

    comm.sendVectorCount(static_cast<uint32_t>(vectors.size()));
    
    std::vector<int32_t> results;
    results.reserve(vectors.size());
    for (const auto& vec : vectors) {
        comm.sendVector(vec);
        int32_t result = comm.receiveResult();
        results.push_back(result);
    }

    DataWriter writer(params.outputFile);
    writer.saveResults(results);

    comm.closeConnection();
    std::cout << "Работа завершена успешно. Результаты сохранены в " << params.outputFile << std::endl;
}