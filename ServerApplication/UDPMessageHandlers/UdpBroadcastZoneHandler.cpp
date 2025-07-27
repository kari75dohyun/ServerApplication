#include "../UDPMessageHandlers/UdpBroadcastZoneHandler.h"
#include "../Session.h"
#include "../DataHandler.h"
#include "../Logger.h"
#include "../Utility.h"
#include "../AppContext.h"

void Udpbroadcastzone_handler(std::shared_ptr<Session> session, const nlohmann::json& msg,
    const boost::asio::ip::udp::endpoint& from, boost::asio::ip::udp::socket& udp_socket, DataHandler* handler_)
{
    if (!session) return;
    int zone_id = session->get_zone_id();
    if (auto handler = handler_) { // handler_는 DataHandler*
        auto zone = handler->get_zone(zone_id);
        if (zone) {
            // 브로드캐스트 내용 구성 (token 등은 제외!)
            nlohmann::json out;
            out["type"] = "broadcast_udp_zone";
            out["nickname"] = session->get_nickname();
            out["msg"] = msg.value("msg", "");
            std::string out_str = out.dump();

            zone->for_each_session([&](const std::shared_ptr<Session>& s) {
                if (!s) return; // 세션 만료

                if (s->get_nickname() == session->get_nickname())
                    return; // 자기자신 제외

                if (auto ep = s->get_udp_endpoint()) {
                    auto data = std::make_shared<std::string>(out_str);
                    udp_socket.async_send_to(
                        boost::asio::buffer(*data), *ep,
                        [data](const boost::system::error_code&, std::size_t) {});
                }
            });

        }
        else {
            AppContext::instance().logger->warn("[UDP] Zone {} not found for broadcast_udp_zone", zone_id);
        }
    }
}