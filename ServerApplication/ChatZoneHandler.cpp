#include "../MessageHandlers/ChatZoneHandler.h"
#include "../Session.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"


void chatzone_handler(std::shared_ptr<Session> session,
    const nlohmann::json& msg,
    DataHandler* handler)
{
    if (!session || !handler) return;

    const int zone_id = session->get_zone_id();
    if (zone_id <= 0) {
        // 존 미배정
        nlohmann::json err = { {"type","error"}, {"msg","not in any zone"} };
        session->post_write(err.dump() + "\n");
        return;
    }

    // 메시지 추출/검증
    std::string chat_msg = msg.value("msg", "");
    std::string nickname = session->get_nickname();

    if (chat_msg.empty()) {
        nlohmann::json err = { {"type","error"}, {"msg","empty message"} };
        session->post_write(err.dump() + "\n");
        return;
    }

    nlohmann::json send_msg;
    send_msg["type"] = "chat_zone";
    send_msg["from"] = nickname;
    send_msg["msg"] = chat_msg;

    // 같은 존에만 TCP 브로드캐스트
    handler->broadcast_zone_tcp(send_msg.dump() + "\n", zone_id, session->get_session_id(), session);

    // 송신자에게는 완료 메시지(또는 echo)
    session->post_write(
        nlohmann::json{
            {"type", "notice"},
            {"msg", chat_msg + " 'Message tcp_zone_sent completed.' "}
        }.dump() + "\n"
    );
}
