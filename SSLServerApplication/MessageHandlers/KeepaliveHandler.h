#pragma once
#include <memory>
#include <nlohmann/json.hpp>

class SSLSession;

void keepalive_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg);