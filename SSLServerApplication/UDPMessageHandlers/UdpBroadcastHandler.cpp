#include "../UDPMessageHandlers/UdpBroadcastHandler.h"
#include "../SSLSession.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"


void Udpbroadcast_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
    const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket, DataHandler* handler)
{
    if (handler) {
        std::string sender_nickname = msg.value("nickname", "");
        handler->udp_broadcast(msg.dump(), udp_socket, sender_nickname);
    };
}