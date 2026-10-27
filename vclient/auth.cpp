#include "auth.h"
#include <cryptopp/sha.h>
#include <cryptopp/hex.h>
#include <cryptopp/filters.h>

std::string Authenticator::computeSHA256(const std::string& salt, const std::string& pass) {
    CryptoPP::SHA256 sha256;
    std::string hashHex;
    
    std::string data = salt + pass;
    
    CryptoPP::StringSource(data, true,
        new CryptoPP::HashFilter(sha256,
            new CryptoPP::HexEncoder(
                new CryptoPP::StringSink(hashHex),
                true
            )
        )
    );
    return hashHex;
} 