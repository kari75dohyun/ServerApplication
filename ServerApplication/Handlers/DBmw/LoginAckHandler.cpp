#include "../../DBmwHandlerRegistry.hpp"
#include "../../DBmwRouter.h"
#include "../../AppContext.h"
#include <nlohmann/json.hpp>

static void OnServerLoginAck(DBmwRouter& router, const nlohmann::json& j) {
    const auto result = j.value("result", std::string());
    const auto nickname = j.value("nickname",
        AppContext::instance().config.value("dbmw_login_nickname", std::string("")));

    if (result == "OK") {
        AppContext::instance().logger->info("[DBMW][RX] server_login_ack OK (nickname={})", nickname);

        if (auto client = AppContext::instance().db_client) {
            client->mark_authed(true);  // 하트비트 활성화
        }

        // (선택) 서버 클라에게 공지
        //if (auto dh = AppContext::instance().data_handler.lock()) {
        //    nlohmann::json out = { {"type","notice"}, {"msg","DB middleware authenticated"} };
        //    dh->broadcast_strict(out.dump());
        //}

        // (선택) 인증 완료시 후속 로직 (예: 상태 플래그/프로브 등) 필요하면 여기서.
    }
    else {
        AppContext::instance().logger->warn("[DBMW][RX] server_login_ack NG: {}", j.dump());
        if (auto client = AppContext::instance().db_client) {
            client->mark_authed(false);
            client->stop();   // 소켓/타이머 닫기
            client->start();  // 즉시 재연결 시도
        }
    }
}

REGISTER_DBMW_HANDLER("server_login_ack", OnServerLoginAck)
