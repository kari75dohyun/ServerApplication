#include "Logger.h"
#include <vector>

std::shared_ptr<spdlog::logger> g_logger = nullptr;

void init_logger() {
    if (g_logger) return; // 중복생성 방지

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("server.log", false);
	// 콘솔 창에도 찍히게 하고 싶을 때는 아래 주석을 해제하세요!
    //auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    std::vector<spdlog::sink_ptr> sinks{ file_sink }; // console_sink }; 주석된거 추가 하면 콘솔창에 로그 추가
    g_logger = std::make_shared<spdlog::logger>("server", sinks.begin(), sinks.end());

    spdlog::register_logger(g_logger);
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    g_logger->set_level(spdlog::level::info);
    g_logger->flush_on(spdlog::level::info);   // info 레벨 이상의 로그는 자동 flush!
}


//#include <spdlog/spdlog.h>
//#include <spdlog/sinks/basic_file_sink.h>
//#include <spdlog/sinks/stdout_color_sinks.h>
//#include <memory>
//#include <vector>
//#include "Logger.h"
//
//void init_logger() {
//    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("server.txt", true);
//    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
//
//    std::vector<spdlog::sink_ptr> sinks{ file_sink, console_sink };
//    auto logger = std::make_shared<spdlog::logger>("server", sinks.begin(), sinks.end());
//    spdlog::register_logger(logger);
//
//    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
//    logger->set_level(spdlog::level::info);
//}
//
//// 전역 g_logger 객체
//std::shared_ptr<spdlog::logger> g_logger = []() {
//    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("server.log", true);
//    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
//
//    std::vector<spdlog::sink_ptr> sinks{ file_sink, console_sink };
//    auto logger = std::make_shared<spdlog::logger>("server", sinks.begin(), sinks.end());
//    spdlog::register_logger(logger);
//
//    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
//    logger->set_level(spdlog::level::info);
//    return logger;
//    }();
