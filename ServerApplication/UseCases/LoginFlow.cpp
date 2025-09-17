#include "LoginFlow.h"
#include "../Session.h"
#include "../AppContext.h"
#include "../DBmwRouter.h"
#include "../DataHandler.h"
#include "../SessionManager.h"
#include "../Zone.h"
#include "../ZoneManager.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void LoginFlow::begin(std::shared_ptr<Session> sess, const std::string& nickname) {
    auto flow = std::shared_ptr<LoginFlow>(new LoginFlow(sess, nickname));
    flow->request_user_info();
}

LoginFlow::LoginFlow(std::shared_ptr<Session> s, std::string n)
    : sess_(s), nickname_(std::move(n)) {
}

void LoginFlow::request_user_info() {
    auto router = AppContext::instance().db_router.lock();
    if (!router) {
        on_user_info_fail(json{ {"type","error"},{"msg","db_router_unavailable"} });
        return;
    }

    auto s = sess_.lock();
    if (!s) return;

    auto self = shared_from_this();
    router->send_request_cb(
        "request_user_info",
        json{ {"nickname", nickname_} },
        /*secure*/ true,
        /*timeout_ms*/ 2500,
        /*max_retries*/ 2,
        /*session_id*/ s->get_session_id(),
        // on_ok
        [self](const json& j) { self->on_user_info_ok(j); },
        // on_error
        [self](const json& j) {
            self->on_user_info_fail(j.value("msg", "timeout"));
        }
    );

    // 필요하면 “조회 중” 안내
    // s->post_write(R"({"type":"notice","msg":"checking account..."})");
}

void LoginFlow::on_user_info_ok(const json& j) {
    // 기대 스키마: {"type":"db_query_result","api":"request_user_info","code":0,"data":{...},"req_id":"..."}
    int code = j.value("code", -1);
    if (code != 0) {
        complete_fail("user_not_found");
        return;
    }

    auto s = sess_.lock();
    if (!s) return;

    // 여기서 DB data 검증/필드 추출 가능
    // const auto& data = j.value("data", json::object());

    // 닉네임 등록 및 중복 로그인 처리
    auto sm = AppContext::instance().session_manager.lock();
    auto dh = AppContext::instance().data_handler.lock();
    if (!sm || !dh) { complete_fail("server_unavailable"); return; }

    // 중복 로그인 축출
    if (auto prev = sm->find_session_by_nickname(nickname_)) {
        if (prev != s) {
            prev->post_write(R"({"type":"error","msg":"Duplicate login ban"})");
            prev->close_session();
        }
    }

    // 등록
    sm->register_nickname(nickname_, s);
    s->set_nickname(nickname_);
    s->on_nickname_registered();

    // 존 배정(정책에 맞게)
    int default_zone = 1;
    dh->assign_session_to_zone(s, default_zone);

    // 입장 공지
    json notice{
        {"type","notice"},
        {"msg", nickname_ + " has entered."}
    };
    dh->broadcast(notice.dump() + "\n", s->get_session_id(), s);

    // 클라 응답
    complete_success();

    //AppContext::instance().logger->info("[on_login] {} 세션 등록 시도", nickname);
    //session_manager->register_nickname(nickname, session);
    //AppContext::instance().logger->info("[on_login] 세션 등록 완료 후 login_success 전송: {}", nickname);

    //session->set_nickname(nickname);
    //session->on_nickname_registered(); // 닉네임 등록시 타이머 중지

    //// zone 배정
    //int default_zone = 1; // 정책에 따라 1번 존에 자동 배정
    //handler->assign_session_to_zone(session, default_zone);

    //nlohmann::json notice;
    //notice["type"] = "notice";
    //notice["msg"] = nickname + " has entered.";
    //handler->broadcast(notice.dump() + "\n", session->get_session_id(), session);

    //nlohmann::json login_msg;
    //login_msg["type"] = "login_success";
    //login_msg["nickname"] = nickname;
    //session->post_write(login_msg.dump() + "\n");
}

void LoginFlow::on_user_info_fail(const nlohmann::json& j) {
    complete_fail(j.is_string() ? j.get<std::string>() : "db_error");
}

void LoginFlow::complete_success() {
    if (auto s = sess_.lock()) {
        json login_msg{
            {"type","login_success"},
            {"nickname", nickname_}
        };
        s->post_write(login_msg.dump() + "\n");
    }
}

void LoginFlow::complete_fail(const std::string& reason) {
    if (auto s = sess_.lock()) {
        json out{
            {"type","login_result"},
            {"ok", false},
            {"error", reason}
        };
        s->post_write(out.dump() + "\n");
        // 실패 시 세션을 종료할지 말지는 정책에 따라
        // s->close_session();
    }
}
