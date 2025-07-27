#pragma once
#include <string>
#include <optional>

// 데이터 나눠 받기 위한 메시지 버퍼 관리 클래스 그리고 패킷 첫 부분에 사이즈 검출
class MessageBufferManager {
    std::string buffer_;
public:
    void append(const char* data, size_t len);
    std::optional<std::string> extract_message();
    void clear();
};