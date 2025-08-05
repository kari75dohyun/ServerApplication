#include "MessageBufferManager.h"
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
//#include <arpa/inet.h> // 리눅스용 (윈도는 winsock2.h에서 ntohl)
#endif
#include <iostream>


void MessageBufferManager::append(const char* data, size_t len) {
    //std::cout << "[서버 누적] 수신 " << len << " bytes: ";
    //for (size_t i = 0; i < len; ++i) {
    //    printf("%02X ", (unsigned char)data[i]);
    //}
    //std::cout << std::endl;

    buffer_.append(data, len);
}

std::optional<std::string> MessageBufferManager::extract_message() {
    constexpr uint32_t MAX_PACKET_SIZE = 4096;  // 원하는 크기로 설정
    last_clear_by_invalid_length_ = false;      // 호출 시마다 초기화

    if (buffer_.size() < 4) return std::nullopt;
    uint32_t len;
    memcpy(&len, buffer_.data(), 4);
    len = ntohl(len);

    // 길이 유효성 검사 추가!
    if (len == 0 || len > MAX_PACKET_SIZE) {
        // 비정상 패킷 길이 → 방어 코드!
        buffer_.clear();  // 버퍼 파기 (DoS 방지)
        // 추가: 로그 남기기(이 함수에 logger 접근권한 없으면 호출부에서)
        last_clear_by_invalid_length_ = true;  // 비정상 길이 감지!
        return std::nullopt;
    }

    if (buffer_.size() < 4 + len) return std::nullopt;
    std::string msg = buffer_.substr(4, len);
    buffer_ = buffer_.substr(4 + len);
    return msg;
}

void MessageBufferManager::clear() { buffer_.clear(); }
