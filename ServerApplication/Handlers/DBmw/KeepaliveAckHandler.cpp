#include "../../DBmwHandlerRegistry.hpp"
#include "../../DBmwRouter.h"
#include "../../AppContext.h"
#include <nlohmann/json.hpp>

// DB 미들웨어로부터 keepalive_ack 를 받았을 때 호출되는 핸들러
static void OnServerKeepAliveAck(DBmwRouter& /*router*/, const nlohmann::json& /*j*/) {
    // 하트비트 ACK를 받았으니 미수신 카운터를 리셋
    if (auto client = AppContext::instance().db_client) {
        client->note_heartbeat_ack();
    }

    // (원하면 로그도 추가 가능)
    AppContext::instance().logger->info("[DBMW][RX] keepalive_ack 수신 → heartbeat OK");
}

// "keepalive_ack" 타입에 이 핸들러 등록
REGISTER_DBMW_HANDLER("keepalive_ack", OnServerKeepAliveAck)
