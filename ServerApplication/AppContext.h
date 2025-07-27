#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

class AppContext {
public:
    std::shared_ptr<spdlog::logger> logger;
    nlohmann::json config;

    static AppContext& instance() {
        static AppContext ctx;
        return ctx;
    }

private:
    AppContext() = default;
};
