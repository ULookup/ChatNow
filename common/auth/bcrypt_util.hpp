#pragma once

#include "bcrypt/bcrypt.h"
#include <string>
#include <stdexcept>

namespace chatnow::auth {

inline std::string hash_password(const std::string& pw) {
    if (pw.empty()) {
        throw std::invalid_argument("password must not be empty");
    }
    char salt[BCRYPT_HASHSIZE];
    char hash[BCRYPT_HASHSIZE];
    if (bcrypt_gensalt(10, salt) != 0) {
        throw std::runtime_error("bcrypt_gensalt failed");
    }
    if (bcrypt_hashpw(pw.c_str(), salt, hash) != 0) {
        throw std::runtime_error("bcrypt_hashpw failed");
    }
    return std::string(hash);
}

inline bool check_password(const std::string& pw, const std::string& hash) {
    if (pw.empty() || hash.empty()) return false;
    return bcrypt_checkpw(pw.c_str(), hash.c_str()) == 0;
}

}  // namespace chatnow::auth
