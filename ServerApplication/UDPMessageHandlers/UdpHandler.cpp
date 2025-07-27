#include "../UDPMessageHandlers/UdpHandler.h"
#include "../Session.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"
#include "../AppContext.h"


void Udp_handler(std::shared_ptr<Session> session, const nlohmann::json& msg,
    const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket)
{
    // Echo 응답 (닉네임/메시지 포함)
    nlohmann::json response;
    response["type"] = "udp_reply";
    response["msg"] = "Echo(UDP): " + msg.value("msg", "");
    response["nickname"] = msg.value("nickname", "anonymity");
    auto data = std::make_shared<std::string>(response.dump());
    udp_socket.async_send_to(
        boost::asio::buffer(*data), from,
        [data](const boost::system::error_code& ec, std::size_t bytes) {
            if (ec) AppContext::instance().logger->warn("[UDP][send_to callback] Error 4: {}", ec.message());
        });
}