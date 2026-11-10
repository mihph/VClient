#pragma once
#include <string>
#include <vector>
#include <cstdint>

class Communicator {
private:
    int sockFd;
    void sendAll(const void* buf, size_t len);
    void recvAll(void* buf, size_t len);
    void sendString(const std::string& msg);
    std::string recvString();
public:
    Communicator() : sockFd(-1) {}
    ~Communicator();
    
    void connectServer(const std::string& ip, int port);
    void closeConnection();
    
    void sendLogin(const std::string& login);
    std::string receiveSalt();
    void sendHash(const std::string& hash);
    std::string receiveAuthStatus();
    
    void sendVectorCount(uint32_t count);
    void sendVector(const std::vector<int32_t>& vec);
    int32_t receiveResult();
}; 