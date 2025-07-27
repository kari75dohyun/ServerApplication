// MemoryTracker.h
#pragma once
#include <string>

class MemoryTracker {
public:
    // 메모리 사용량 정보를 반환 (VmSize, VmRSS 등)
    static std::string get_memory_usage();

    // 로거로 직접 남기는 함수
    static void log_memory_usage();
};

