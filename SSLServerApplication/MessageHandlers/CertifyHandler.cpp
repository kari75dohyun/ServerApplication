#include "../MessageHandlers/CertifyHandler.h"
#include "../SSLSession.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"
#include "../SessionManager.h"
#include <iostream>


void login_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
    DataHandler* handler, SessionManager* session_manager)
{
    std::string nickname = msg.value("nickname", "anonymity");

    if (nickname == "quit") {
        session->post_write(R"({"type":"logout","msg":"You are not logged in."})" "\n");
        g_logger->warn("[Connection reset] Client disconnected without a nickname. session_id : {} ", session->get_session_id());
        std::cerr << "Client disconnected without a nickname. " << " session_id: " << session->get_session_id() << std::endl;
        session->close_session();
        return;
    }

    // [중복 검사/처리]
    auto prev = session_manager->find_session_by_nickname(nickname);
    if (prev && prev != session) {
        prev->post_write(R"({"type":"error","msg":"Duplicate login ban"})" "\n");
        prev->close_session();
    }
    //handler_->register_nickname(nickname, session);
    g_logger->info("[on_login] {} 세션 등록 시도", nickname);
    session_manager->register_nickname(nickname, session);
    g_logger->info("[on_login] 세션 등록 완료 후 login_success 전송: {}", nickname);

    session->set_nickname(nickname);
    session->on_nickname_registered(); // 닉네임 등록시 타이머 중지

    // zone 배정
    int default_zone = 1; // 정책에 따라 1번 존에 자동 배정
    handler->assign_session_to_zone(session, default_zone);

    nlohmann::json notice;
    notice["type"] = "notice";
    notice["msg"] = nickname + " has entered.";
    handler->broadcast(notice.dump() + "\n", session->get_session_id(), session);

    nlohmann::json login_msg;
    login_msg["type"] = "login_success";
    login_msg["nickname"] = nickname;
    session->post_write(login_msg.dump() + "\n");
}

void logout_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg, DataHandler* handler)
{
    std::string nickname = session->get_nickname();
    if (nickname.empty()) {
        session->post_write(R"({"type":"logout","msg":"You are not logged in."})" "\n");
        g_logger->warn("[Connection reset] Client disconnected without a nickname. session_id : {} ", session->get_session_id());
        std::cerr << "Client disconnected without a nickname. " << " session_id: " << session->get_session_id() << std::endl;
        session->close_session();
        return;
    }
    nlohmann::json notice;
    notice["type"] = "notice";
    notice["msg"] = nickname + " has left.";
    handler->broadcast(notice.dump() + "\n", session->get_session_id(), session);
    // 세션 종료
    session->post_write(R"({"type":"logout","msg":"You quit normally."})" "\n");
    g_logger->warn("[Connection reset] Client disconnected. nickname: {} session_id : {}", nickname, session->get_session_id());
    std::cerr << "Client disconnected. " << "  nickname: " << nickname << "  session_id: " << session->get_session_id() << std::endl;
    session->close_session();
}