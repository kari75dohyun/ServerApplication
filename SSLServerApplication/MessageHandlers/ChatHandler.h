#pragma once
#include <memory>
#include <nlohmann/json.hpp>

class SSLSession;
class DataHandler;

void chat_handler(std::shared_ptr<SSLSession> session,
    const nlohmann::json& msg,
    DataHandler* handler);