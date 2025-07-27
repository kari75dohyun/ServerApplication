#include "Logger.h"
#include <vector>
#include "AppContext.h"


void init_logger() {
    if (!AppContext::instance().logger) {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("server.log", false);

        //콘솔창에 로그 찍고 싶으면 아래 주석 풀기
        //auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        //std::vector<spdlog::sink_ptr> sinks{ file_sink, console_sink };

        std::vector<spdlog::sink_ptr> sinks{ file_sink };
        auto logger = std::make_shared<spdlog::logger>("server", sinks.begin(), sinks.end());

        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);

        spdlog::register_logger(logger);
        AppContext::instance().logger = logger;
    }
}
