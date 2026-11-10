#include "client.h"
#include "interface.h"
#include <iostream>

int main(int argc, char** argv) {
    try {
        UserInterface ui;
        if (!ui.parseArgs(argc, argv)) {
            return 0;
        }
        
        Params params = ui.getParams();
        Client client(params);
        return client.start(argc, argv);
        
    } catch (const po::error& e) {
        std::cerr << "Ошибка параметров командной строки: " << e.what() << std::endl;
        std::cerr << "Используйте -h для получения справки." << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Критическая ошибка инициализации: " << e.what() << std::endl;
        return 2;
    }
}