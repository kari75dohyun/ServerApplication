#pragma once
#include <memory>
#include <nlohmann/json.hpp>

class Session;
class DataHandler;

void chat_handler(std::shared_ptr<Session> session,
    const nlohmann::json& msg,
    DataHandler* handler);