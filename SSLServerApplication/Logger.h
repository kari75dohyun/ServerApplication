#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>

extern std::shared_ptr<spdlog::logger> g_logger;

void init_logger();

//#include <fstream>
//#include <mutex>
//#include <string>
//#include <sstream>
//#include <chrono>
//#include <iomanip>

//inline std::tm* get_local_time(const std::time_t* t) {
//    static thread_local std::tm tm_buf;
//#ifdef _WIN32
//    localtime_s(&tm_buf, t);
//#else
//    localtime_r(t, &tm_buf);
//#endif
//    return &tm_buf;
//}
//
//// ===== Logger 클래스 선언/정의 =====
//class Logger {
//public:
//    Logger(const std::string& filename = "server.log")
//        : log_file_(filename, std::ios::app) {
//    }
//
//    // 로그 레벨별 메서드
//    void info(const std::string& msg) { write("INFO", msg); }
//    void warn(const std::string& msg) { write("WARN", msg); }
//    void error(const std::string& msg) { write("ERROR", msg); }
//	void debug(const std::string& msg) { write("DEBUG", msg); }
//
//private:
//    std::ofstream log_file_;
//    std::mutex mutex_;
//
//    void write(const char* level, const std::string& msg) {
//        std::lock_guard<std::mutex> lock(mutex_);
//        auto now = std::chrono::system_clock::now();
//        auto time = std::chrono::system_clock::to_time_t(now);
//        log_file_ << "[" << level << "] "
//            << std::put_time(get_local_time(&time), "%F %T") << " "
//            << msg << std::endl;
//    }
//};
//
//// ===== log_line 유틸 =====
//template<typename... Args>
//inline std::string log_line(Args&&... args) {
//    std::ostringstream oss;
//    (oss << ... << args);
//    return oss.str();
//}
//
//// ====== 전역 Logger 객체 (싱글톤 스타일) ======
//inline Logger g_logger("server.log");
//
//// ====== 매크로 정의 (한줄 로그) ======
//#define LOG_INFO(...)   g_logger.info(log_line(__VA_ARGS__))
//#define LOG_WARN(...)   g_logger.warn(log_line(__VA_ARGS__))
//#define LOG_ERROR(...)  g_logger.error(log_line(__VA_ARGS__))
//#define LOG_DEBUG(...)  g_logger.debug(log_line(__VA_ARGS__))
//
//// 사용 예시:
//// LOG_INFO("[add_session] session_id=", session_id, " added to shard ", shard);
