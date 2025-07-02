#pragma once
#include <functional>
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>

class SSLSession;
class DataHandler;

class MessageDispatcher {
public:
    using HandlerFunc = std::function<void(std::shared_ptr<SSLSession>, const nlohmann::json&)>;

    MessageDispatcher(DataHandler* handler); // DataHandler 포인터 주입

    void dispatch(std::shared_ptr<SSLSession> session, const nlohmann::json& msg);

    void register_handler(const std::string& type, HandlerFunc handler);

private:
    std::unordered_map<std::string, HandlerFunc> handlers_;
    DataHandler* handler_;
};
