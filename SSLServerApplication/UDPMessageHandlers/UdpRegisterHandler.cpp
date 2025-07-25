#include "../UDPMessageHandlers/UdpRegisterHandler.h"
#include "../SSLSession.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"


void Udpregister_handler(std::shared_ptr<SSLSession> session, const nlohmann::json& msg,
    const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket)
{
    // 별도 응답 필요시 이 곳에서 UDP로 전송
    // 랜덤 토큰 생성 및 저장
    std::string udp_token = generate_random_token();
    session->set_udp_token(udp_token);

    nlohmann::json response;
    response["type"] = "udp_register_ack";
    response["msg"] = "UDP 인증 토큰 발급 및 endpoint registered";
    response["token"] = udp_token;
    response["nickname"] = msg.value("nickname", "anonymity");

    g_logger->info("[DEBUG][UDP] 응답 전송 - endpoint: {}:{}", from.address().to_string(), from.port());
    auto data = std::make_shared<std::string>(response.dump());
    udp_socket.async_send_to(
        boost::asio::buffer(*data), from,
        [data](const boost::system::error_code& ec, std::size_t bytes) {
            if (ec) g_logger->error("[UDP][send_to callback] Error 5: {}", ec.message());
        });
}