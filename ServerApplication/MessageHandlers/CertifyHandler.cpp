#include "../MessageHandlers/CertifyHandler.h"
#include "../Session.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"
#include "../SessionManager.h"
#include <iostream>
#include "../AppContext.h"
#include "../DBmwRouter.h"
#include "../UseCases/LoginFlow.h"


void login_handler(std::shared_ptr<Session> session, const nlohmann::json& msg,
    DataHandler* handler, SessionManager* session_manager)
{
    std::string nickname = msg.value("nickname", "anonymity");
    AppContext::instance().logger->info("[DEBUG] login_handler called: session_id={}", session->get_session_id());
    if (nickname.empty()) {
        session->post_write(R"({"type":"error","msg":"nickname required"})");
        return;
    }

    if (nickname == "quit") {
        session->post_write(R"({"type":"logout","msg":"You are not logged in."})" "\n");
        AppContext::instance().logger->warn("[Connection reset] Client disconnected without a nickname. session_id : {} ", session->get_session_id());
        std::cerr << "Client disconnected without a nickname. " << " session_id: " << session->get_session_id() << std::endl;
        session->close_session();
        return;
    }

    // [중복 검사/처리]
    auto prev = session_manager->find_session_by_nickname(nickname);
    if (prev && prev != session) {
        prev->post_write(R"({"type":"error","msg":"Duplicate login ban"})" "\n");
        prev->close_session();   // 기존 사용자를 삭제 한다.
    }

    LoginFlow::begin(session, nickname);
}

void logout_handler(std::shared_ptr<Session> session, const nlohmann::json& msg, DataHandler* handler)
{
    std::string nickname = session->get_nickname();
    if (nickname.empty()) {
        session->post_write(R"({"type":"logout","msg":"You are not logged in."})" "\n");
        AppContext::instance().logger->warn("[Connection reset] Client disconnected without a nickname. session_id : {} ", session->get_session_id());
        std::cerr << "Client disconnected without a nickname. " << " session_id: " << session->get_session_id() << std::endl;
        session->close_session();
        return;
    }
    nlohmann::json notice;
    notice["type"] = "notice";
    notice["msg"] = nickname + " has left.";
    handler->broadcast(notice.dump() + "\n", session->get_session_id(), session);
    // 세션 종료
    //session->post_write(R"({"type":"logout","msg":"You quit normally."})" "\n");
    AppContext::instance().logger->warn("[Connection reset] Client disconnected. nickname: {} session_id : {}", nickname, session->get_session_id());
    std::cerr << "Client disconnected. " << "  nickname: " << nickname << "  session_id: " << session->get_session_id() << std::endl;
    session->close_session();
}