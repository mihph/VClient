#include "communicator.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdexcept>
#include <memory>
#include <cstring>
#include <cerrno>

Communicator::~Communicator() { closeConnection(); }

void Communicator::connectServer(const std::string& ip, int port) {
    sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd == -1) {
        throw std::system_error(errno, std::generic_category(), "socket() failed");
    }

    struct timeval timeout{180, 0};
    setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::unique_ptr<sockaddr_in> servAddr(new sockaddr_in);
    servAddr->sin_family = AF_INET;
    servAddr->sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &servAddr->sin_addr) <= 0) {
        throw std::runtime_error("Неверный IP адрес: " + ip);
    }

    if (connect(sockFd, reinterpret_cast<sockaddr*>(servAddr.get()), sizeof(sockaddr_in)) == -1) {
        throw std::system_error(errno, std::generic_category(), "connect() failed to " + ip + ":" + std::to_string(port));
    }
}

void Communicator::sendAll(const void* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sockFd, static_cast<const char*>(buf) + sent, len - sent, 0);
        if (n <= 0) {
            throw std::system_error(errno, std::generic_category(), "send() failed");
        }
        sent += n;
    }
}

void Communicator::recvAll(void* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(sockFd, static_cast<char*>(buf) + received, len - received, 0);
        if (n <= 0) {
            throw std::system_error(errno, std::generic_category(), "recv() failed or timeout");
        }
        received += n;
    }
}

void Communicator::sendString(const std::string& msg) {
    sendAll(msg.c_str(), msg.length());
}

std::string Communicator::recvString() {
    char buffer[1024];
    ssize_t n = recv(sockFd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        throw std::system_error(errno, std::generic_category(), "recvString() failed");
    }
    buffer[n] = '\0';
    std::string res(buffer);
    while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) {
        res.pop_back();
    }
    return res;
}

void Communicator::sendLogin(const std::string& login) { sendString(login); }
std::string Communicator::receiveSalt() { return recvString(); }
void Communicator::sendHash(const std::string& hash) { sendString(hash); }
std::string Communicator::receiveAuthStatus() { return recvString(); }

void Communicator::sendVectorCount(uint32_t count) {
    uint32_t netCount = htonl(count);
    sendAll(&netCount, sizeof(netCount));
}

void Communicator::sendVector(const std::vector<int32_t>& vec) {
    uint32_t size = htonl(static_cast<uint32_t>(vec.size()));
    sendAll(&size, sizeof(size));
    
    for (int32_t val : vec) {
        uint32_t netVal = htonl(static_cast<uint32_t>(val));
        sendAll(&netVal, sizeof(netVal));
    }
}

int32_t Communicator::receiveResult() {
    uint32_t netRes;
    recvAll(&netRes, sizeof(netRes));
    return static_cast<int32_t>(ntohl(netRes));
}

void Communicator::closeConnection() {
    if (sockFd != -1) {
        close(sockFd);
        sockFd = -1;
    }
}