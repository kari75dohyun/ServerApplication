#include "MessageBufferManager.h"
#include <cstring>
#define NOMINMAX
#include <algorithm>
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
    constexpr uint32_t MAX_PACKET_SIZE = 64 * 4096;  // 원하는 크기로 설정
    constexpr size_t  MAX_RESYNC_SCAN = 4096;      // resync 시 최대 검색 길이(버퍼 앞부분만 스캔)

    last_clear_by_invalid_length_ = false;      // 호출 시마다 초기화

    // 최소 4바이트(길이) 없으면 대기
    if (buffer_.size() < 4) return std::nullopt;

    auto read_len_at = [&](size_t offset) -> uint32_t {
        uint32_t len_net;
        std::memcpy(&len_net, buffer_.data() + offset, 4);
        return ntohl(len_net);
        };

    // 1. 현재 위치(0)에서 정상 길이인지 확인
    uint32_t len_0 = read_len_at(0);
    if (len_0 > 0 && len_0 <= MAX_PACKET_SIZE) {
        if (buffer_.size() >= 4 + len_0) {
            std::string msg = buffer_.substr(4, len_0);
            buffer_.erase(0, 4 + len_0);
            return msg;
        }
        // 길이는 유효하지만 아직 데이터가 부족 -> 더 기다림
        return std::nullopt;
    }

    // 2. 현재 프리픽스가 비정상일 경우: resync 시도
    // 스캔은 버퍼 앞부분(MAX_RESYNC_SCAN 또는 buffer_.size())까지만 수행해 비용 제한
    constexpr size_t kMaxResyncScan = 16384;               // (원하는 값) 상수 예시
    size_t scan_limit = (std::min)(buffer_.size(), static_cast<size_t>(kMaxResyncScan + 4));
    for (size_t i = 1; i + 4 <= scan_limit; ++i) {
        uint32_t cand = read_len_at(i);
        if (cand > 0 && cand <= MAX_PACKET_SIZE) {
            // 후보 프리픽스 발견. 전체 메시지가 이미 수신되었는지 확인
            if (buffer_.size() >= i + 4 + static_cast<size_t>(cand)) {
                // 앞부분 잡음 제거하고 메시지 추출
                buffer_.erase(0, i); // 이제 새로운 시작은 valid 프리픽스
                std::string msg = buffer_.substr(4, cand);
                buffer_.erase(0, 4 + cand);
                return msg;
            }
            else {
                // 후보 프리픽스는 유효하지만 아직 전송 완료 아님.
                // 앞부분 잡음만 제거해서 resync(대기)
                buffer_.erase(0, i);
                return std::nullopt;
            }
        }
    }

    // 3. 여기까지 왔으면 스캔 실패. 버퍼가 과도하게 크면 안전하게 폐기.
    // 그렇지 않으면 더 많은 데이터가 들어오기를 기다림(무조건 제거하지 않음).
    constexpr size_t ABSOLUTE_MAX_BUFFER = 512 * 1024; // 512KB: 버퍼가 이보다 커지면 방어적으로 비움
    if (buffer_.size() > ABSOLUTE_MAX_BUFFER) {
        buffer_.clear();
        last_clear_by_invalid_length_ = true;
        return std::nullopt;
    }

    // 또한, 만약 초기 4바이트가 명백히 비정상(예: 0 혹은 너무 큼)이고
    // buffer가 일정 크기 이상 모였으면 앞부분 일부를 잘라내는 게 안전할 수 있음.
    // 여기서는 비용을 낮게 하기 위해 아직은 기다림으로 처리.
    return std::nullopt;

    //if (buffer_.size() < 4) return std::nullopt;
    //uint32_t len;
    //memcpy(&len, buffer_.data(), 4);
    //len = ntohl(len);

    //// 길이 유효성 검사 추가!
    //if (len == 0 || len > MAX_PACKET_SIZE) {
    //    // 비정상 패킷 길이 -> 방어 코드!
    //    buffer_.clear();  // 버퍼 파기 (DoS 방지)
    //    // 추가: 로그 남기기(이 함수에 logger 접근권한 없으면 호출부에서)
    //    last_clear_by_invalid_length_ = true;  // 비정상 길이 감지!
    //    return std::nullopt;
    //}

    //if (buffer_.size() < 4 + len) return std::nullopt;
    //std::string msg = buffer_.substr(4, len);
    //buffer_ = buffer_.substr(4 + len);
    //return msg;
}

void MessageBufferManager::clear() { buffer_.clear(); }
