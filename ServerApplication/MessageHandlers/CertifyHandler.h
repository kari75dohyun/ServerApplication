#pragma once
#include <memory>
#include <nlohmann/json.hpp>

class Session;
class DataHandler;
class SessionManager;

void login_handler(std::shared_ptr<Session> session, const nlohmann::json& msg,
    DataHandler* handler, SessionManager* session_manager);

void logout_handler(std::shared_ptr<Session> session, const nlohmann::json& msg, DataHandler* handler);
