#include "MessageDispatcher.h"
#include "SSLSession.h"
#include "DataHandler.h"
#include <iostream>
//#include <fstream>

MessageDispatcher::MessageDispatcher(DataHandler* handler) : handler_(handler) {
    // "login" 핸들러 등록
    register_handler("login", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
        std::string nickname = msg.value("nickname", "anonymity");
        session->set_nickname(nickname);

        nlohmann::json notice;
        notice["type"] = "notice";
        notice["msg"] = nickname + " has entered.";
        handler_->broadcast(notice.dump() + "\n", session->get_session_id(), session);

        //session->set_message(R"({"type":"notice","msg":"welcome!"})" "\n");
        //handler_->do_write(session);
        session->post_write(u8R"({"type":"notice","msg":"welcome!"})" "\n");
        });

    // "chat" 핸들러 등록
    register_handler("chat", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
        std::string chat_msg = msg.value("msg", "");
        std::string nickname = session->get_nickname();

        nlohmann::json send_msg;
        send_msg["type"] = "chat";
        send_msg["from"] = nickname;
        send_msg["msg"] = chat_msg;

        // 한글 채팅 데이터 안깨지는 확인
        //std::cout << "HEX: ";
        //for (unsigned char c : chat_msg) printf("%02X ", c);
        //std::cout << std::endl;

        //std::ofstream ofs("chat_msg.txt", std::ios::binary);
        //ofs << chat_msg;
        //ofs.close();

        handler_->broadcast(send_msg.dump() + "\n", session->get_session_id(), session);

        session->post_write(
            nlohmann::json{
                {"type", "notice"},
                {"msg", chat_msg + " 'Message sent completed.' "}
            }.dump() + "\n"
        );
        });
}

void MessageDispatcher::dispatch(std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
    std::string type = msg.value("type", "");
    auto it = handlers_.find(type);
    if (it != handlers_.end()) {
        it->second(session, msg);
    }
    else {
        // Unknown type
        session->post_write(R"({"type":"error","msg":"Unknown message type."})" "\n");
    }
}

void MessageDispatcher::register_handler(const std::string& type, HandlerFunc handler) {
    handlers_[type] = handler;
}
