#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace chatnow {

inline std::string percent_encode_userinfo(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) {
        const bool unreserved =
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '.' || byte == '_' || byte == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0x0F]);
    }
    return encoded;
}

inline std::string make_amqp_url(const std::string& user,
                                 const std::string& password,
                                 const std::string& host,
                                 uint16_t port = 5672) {
    if (host.empty() || host.find_first_of("@:/?#% \t\r\n") != std::string::npos) {
        throw std::invalid_argument("RabbitMQ host must be a hostname without a port");
    }
    return "amqp://" + percent_encode_userinfo(user) + ":" +
           percent_encode_userinfo(password) + "@" + host + ":" +
           std::to_string(port) + "/";
}

}  // namespace chatnow
