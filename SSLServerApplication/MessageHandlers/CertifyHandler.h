#pragma once
#include <memory>
#include <nlohmann/json.hpp>

class SSLSession;
class DataHandler;
class SessionManager;

void login_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
    DataHandler* handler, SessionManager* session_manager);

void logout_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg, DataHandler* handler);
