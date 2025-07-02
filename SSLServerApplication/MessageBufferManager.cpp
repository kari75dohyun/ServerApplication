#include "MessageBufferManager.h"
#include <cstring>
#include <winsock2.h>
#include <iostream>
//#include <arpa/inet.h> // 리눅스용 (윈도는 winsock2.h에서 ntohl)

void MessageBufferManager::append(const char* data, size_t len) {
    //std::cout << "[서버 누적] 수신 " << len << " bytes: ";
    //for (size_t i = 0; i < len; ++i) {
    //    printf("%02X ", (unsigned char)data[i]);
    //}
    //std::cout << std::endl;

    buffer_.append(data, len);
}

std::optional<std::string> MessageBufferManager::extract_message() {
    if (buffer_.size() < 4) return std::nullopt;
    uint32_t len;
    memcpy(&len, buffer_.data(), 4);
    len = ntohl(len);
    if (buffer_.size() < 4 + len) return std::nullopt;
    std::string msg = buffer_.substr(4, len);
    buffer_ = buffer_.substr(4 + len);
    return msg;
}

void MessageBufferManager::clear() { buffer_.clear(); }
