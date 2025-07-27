#pragma once
#include <memory>
#include <nlohmann/json.hpp>

class Session;

void keepalive_handler(std::shared_ptr<Session> session, const nlohmann::json& msg);