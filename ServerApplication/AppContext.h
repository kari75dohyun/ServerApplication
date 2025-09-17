#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "DBMiddlewareClient.h"
#include "SessionManager.h"
#include "DataHandler.h"
#include "DBmwRouter.h"

class AppContext {
public:
    std::shared_ptr<spdlog::logger> logger;
    nlohmann::json config;

    std::shared_ptr<DBMiddlewareClient> db_client;

    std::weak_ptr<DataHandler>    data_handler;
    std::weak_ptr<SessionManager> session_manager;
    std::weak_ptr<DBmwRouter>    db_router;

    static AppContext& instance() {
        static AppContext ctx;
        return ctx;
    }

private:
    AppContext() = default;
};
