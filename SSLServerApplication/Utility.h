#pragma once

#include <string>
#include <random>
#include <sstream>
#include <boost/asio.hpp>
#include <memory>
#include <nlohmann/json.hpp>

// 어디서든 include해서 쓸 수 있도록!
inline std::string generate_random_token(size_t length = 32) {
    static const char hex_chars[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::ostringstream oss;
    for (size_t i = 0; i < length; ++i) {
        oss << hex_chars[dis(gen)];
    }
    return oss.str();
}