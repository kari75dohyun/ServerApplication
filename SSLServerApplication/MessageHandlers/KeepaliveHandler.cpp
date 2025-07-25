#include "../MessageHandlers/KeepaliveHandler.h"
#include "../SSLSession.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"


void keepalive_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg)
{
    //std::cout << "[keepalive] session_id=" << session->get_session_id() << " ← 클라이언트로부터 keepalive 수신" << std::endl;
    g_logger->info("[keepalive] session_id= {} ← 클라이언트로부터 keepalive 수신", session->get_session_id());
    session->update_alive_time();     // 글로벌 keepalive 타이머 갱신
    //session->on_pong_received();   // keepalive session에서 관리 할때 
    // (원하면) 응답 필요 없으면 생략 가능
    // session->post_write(R"({"type":"pong"})" "\n");
}