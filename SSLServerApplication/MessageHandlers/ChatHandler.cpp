#include "../MessageHandlers/ChatHandler.h"
#include "../SSLSession.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"


void chat_handler(std::shared_ptr<SSLSession> session,
    const nlohmann::json& msg,
    DataHandler* handler)
{
    std::string chat_msg = msg.value("msg", "");
    std::string nickname = session->get_nickname();

    nlohmann::json send_msg;
    send_msg["type"] = "chat";
    send_msg["from"] = nickname;
    send_msg["msg"] = chat_msg;

    // 전체에 브로드캐스트 (자신 제외, handler 필요)
    if (handler) {
        handler->broadcast(send_msg.dump() + "\n", session->get_session_id(), session);
    }

    // 송신자에게는 완료 메시지(또는 echo)
    session->post_write(
        nlohmann::json{
            {"type", "notice"},
            {"msg", chat_msg + " 'Message sent completed.' "}
        }.dump() + "\n"
    );
}
