#include "UDPManager.h"
#include "DataHandler.h"
#include <iostream>

using boost::asio::ip::udp;

UDPManager::UDPManager(boost::asio::io_context& io, unsigned short port, std::shared_ptr<DataHandler> data_handler)
    : socket_(io, udp::endpoint(udp::v4(), port)), data_handler_(data_handler) {
    start_receive();
}

void UDPManager::start_receive() {
    socket_.async_receive_from(
        boost::asio::buffer(buffer_), remote_endpoint_,
        [this](const boost::system::error_code& ec, std::size_t bytes_recvd) {
            if (!ec && bytes_recvd > 0) {
                auto msg = std::make_shared<std::string>(buffer_.data(), bytes_recvd);

                if (auto handler = data_handler_.lock()) {
                    // 예시: 클라이언트가 {"type":"broadcast_udp", ...}로 보내면 전체 브로드캐스트
                    try {
                        auto jmsg = nlohmann::json::parse(*msg);
                        if (jmsg.value("type", "") == "broadcast_udp") {
                            // 전체 유저에게 UDP 브로드캐스트
                            handler->udp_broadcast(*msg, socket_);
                        }
                        else {
                            handler->on_udp_receive(*msg, remote_endpoint_, socket_);
                        }
                    }
                    catch (...) {
                        handler->on_udp_receive(*msg, remote_endpoint_, socket_);
                    }
                }
            }
            // 항상 다시 수신 대기
            start_receive();
        });
}

//void UDPManager::start_receive() {
//    socket_.async_receive_from(
//        boost::asio::buffer(buffer_), remote_endpoint_,
//        [this](const boost::system::error_code& ec, std::size_t bytes_recvd) {
//            if (!ec && bytes_recvd > 0) {
//                auto msg = std::make_shared<std::string>(buffer_.data(), bytes_recvd);
//
//                if (auto handler = data_handler_.lock()) {
//                    // 실제 메시지 처리 및 응답 전송은 DataHandler에 맡김
//                    handler->on_udp_receive(*msg, remote_endpoint_, socket_);
//                }
//            }
//            // 항상 다시 수신 대기
//            start_receive();
//        });
//}
