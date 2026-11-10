#include "interface.h"
#include <iostream>
#include <cstdlib>
#include <stdexcept>

UserInterface::UserInterface() : desc("Allowed options") {
    desc.add_options()
        ("help,h", "Show help")
        ("address,a", po::value<std::string>(&params.serverIp)->required(), "Server IP address (required)")
        ("port,p", po::value<int>(&params.serverPort)->default_value(33333), "Server port")
        ("input,i", po::value<std::string>(&params.inputFile)->required(), "Input file (required)")
        ("output,o", po::value<std::string>(&params.outputFile)->required(), "Output file (required)")
        ("config,c", po::value<std::string>(&params.configFile)->default_value("~/.config/vclient.conf"), "Config file");
}

bool UserInterface::parseArgs(int argc, char** argv) {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
        showHelp();
        return false;
    }
    po::notify(vm);
    
    if (!params.configFile.empty() && params.configFile[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            params.configFile.replace(0, 1, std::string(home));
        } else {
            throw std::runtime_error("Не удалось определить домашнюю директорию для пути ~/.config/vclient.conf");
        }
    }
    return true;
}

void UserInterface::showHelp() {
    std::cout << desc << std::endl;
}