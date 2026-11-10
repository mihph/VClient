#pragma once
#include "interface.h"

class Client {
private:
    Params params;
    void handleError(const std::string& func, const std::string& context, const std::exception& e);
public:
    Client(const Params& p) : params(p) {}
    int start(int argc, char** argv);
    void run();
}; 