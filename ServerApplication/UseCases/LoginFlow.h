#pragma once
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

class Session;

class LoginFlow : public std::enable_shared_from_this<LoginFlow> {
public:
    static void begin(std::shared_ptr<Session> sess, const std::string& nickname);

private:
    LoginFlow(std::shared_ptr<Session> s, std::string n);

    void request_user_info();
    void on_user_info_ok(const nlohmann::json& j);
    void on_user_info_fail(const nlohmann::json& j);

    void complete_success();
    void complete_fail(const std::string& reason);

private:
    std::weak_ptr<Session> sess_;
    std::string nickname_;
};
