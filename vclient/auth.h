#pragma once
#include <string>

class Authenticator {
public:
    static std::string computeSHA256(const std::string& salt, const std::string& pass);
};