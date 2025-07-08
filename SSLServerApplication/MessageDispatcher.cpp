#include "MessageDispatcher.h"
#include "SSLSession.h"
#include "DataHandler.h"
#include <iostream>
//#include <fstream>

MessageDispatcher::MessageDispatcher(DataHandler* handler) : handler_(handler) {
    // "login" 핸들러 등록
    register_handler("login", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
        std::string nickname = msg.value("nickname", "anonymity");

        // [중복 검사/처리]
        auto prev = handler_->find_session_by_nickname(nickname);
        if (prev && prev != session) {
            prev->post_write(R"({"type":"error","msg":"다른 곳에서 로그인되어 기존 연결이 종료됩니다."})" "\n");
            prev->close_session();
        }
        handler_->register_nickname(nickname, session);

        session->set_nickname(nickname);
        session->on_nickname_registered(); // 닉네임 등록시 타이머 중지

        nlohmann::json notice;
        notice["type"] = "notice";
        notice["msg"] = nickname + " has entered.";
        handler_->broadcast(notice.dump() + "\n", session->get_session_id(), session);

        //session->set_message(R"({"type":"notice","msg":"welcome!"})" "\n");
        //handler_->do_write(session);
        session->post_write(R"({"type":"notice","msg":"welcome!"})" "\n");
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

    //register_handler("ping", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
    //    session->on_ping_received(); // 아래에서 구현!
    //    // 원하면 "pong" 응답도 가능
    //    session->post_write(R"({"type":"pong"})" "\n");
    //    });

    register_handler("keepalive", [this](std::shared_ptr<SSLSession> session, const nlohmann::json& msg) {
        std::cout << "[keepalive] session_id=" << session->get_session_id() << " ← 클라이언트로부터 keepalive 수신" << std::endl;
		session->update_alive_time();     // 글로벌 keepalive 타이머 갱신
        //session->on_pong_received();   // keepalive session에서 관리 할때 
        // (원하면) 응답 필요 없으면 생략 가능
        // session->post_write(R"({"type":"pong"})" "\n");
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
