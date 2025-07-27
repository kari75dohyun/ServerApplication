#include "UDPManager.h"
#include "DataHandler.h"
#include <iostream>
#include "Logger.h"
#include "AppContext.h"

using boost::asio::ip::udp;

UDPManager::UDPManager(boost::asio::io_context& io, unsigned short port, std::shared_ptr<DataHandler> data_handler)
    : socket_(io, udp::endpoint(udp::v4(), port)), data_handler_(data_handler) {
    start_receive();
}

UDPManager::~UDPManager() {
    if (socket_.is_open()) {
        boost::system::error_code ec;
        socket_.close(ec);
        if (ec) {
            AppContext::instance().logger->error("[UDPManager] 소멸자에서 udp 소켓 close 에러:  {}", ec.message());
        }
    }
}

void UDPManager::start_receive() {
    socket_.async_receive_from(
        boost::asio::buffer(buffer_), remote_endpoint_,
        [this](const boost::system::error_code& ec, std::size_t bytes_recvd) {
            if (!ec && bytes_recvd > 0) {
                auto msg = std::make_shared<std::string>(buffer_.data(), bytes_recvd);

                if (auto handler = data_handler_.lock()) {
                    auto jopt = try_parse_json(*msg);
                    if (!jopt) {
                        AppContext::instance().logger->warn("[UDPManager] Invalid JSON received from {}:{}", remote_endpoint_.address().to_string(), remote_endpoint_.port());

                        // 에러 응답
                        nlohmann::json err_resp = {
                            {"type", "udp_error"},
                            {"error", "Invalid JSON format"}
                        };
                        auto data = std::make_shared<std::string>(err_resp.dump());
                        socket_.async_send_to(boost::asio::buffer(*data), remote_endpoint_,
                            [data](const boost::system::error_code& send_ec, std::size_t) {
                                if (send_ec) {
                                    AppContext::instance().logger->warn("[UDPManager] Error response send failed: {}", send_ec.message());
                                }
                            });

                        // 간단 차단 로직
                        handler->register_bad_udp_packet(remote_endpoint_);
                        if (handler->is_blocked_udp(remote_endpoint_)) {
                            AppContext::instance().logger->warn("[UDPManager] UDP client blocked due to repeated invalid packets: {}:{}", remote_endpoint_.address().to_string(), remote_endpoint_.port());
                        }

                        start_receive();
                        return;
                    }

                    //Token Bucket Rate Limit 검사
                    auto session = handler->getSessionByEndpoint(remote_endpoint_);
                    if (session && !session->checkUdpRateLimit()) {
                        AppContext::instance().logger->warn("[UDPManager] UDP rate limit exceeded for {}", remote_endpoint_.address().to_string());

                        nlohmann::json err_resp = {
                            {"type", "udp_error"},
                            {"error", "너무 빠르게 UDP 메시지를 보내고 있습니다."}
                        };
                        auto data = std::make_shared<std::string>(err_resp.dump());
                        socket_.async_send_to(boost::asio::buffer(*data), remote_endpoint_,
                            [data](const boost::system::error_code& send_ec, std::size_t) {
                                if (send_ec) {
                                    AppContext::instance().logger->warn("[UDPManager] Rate limit error send failed: {}", send_ec.message());
                                }
                            });

                        start_receive();
                        return;
                    }

                    // 정상 처리
                    try {
                        handler->on_udp_receive(*msg, remote_endpoint_, socket_);
                    }
                    catch (const std::exception& e) {
                        AppContext::instance().logger->error("[UDPManager] on_udp_receive 예외 발생: {}", e.what());
                    }
                }
            }
            start_receive(); // 다음 수신 준비
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
//                    auto jopt = try_parse_json(*msg);
//                    if (!jopt) {
//                        // JSON 파싱 실패 처리
//                        AppContext::instance().logger->warn("[UDPManager] Invalid JSON received from {}:{}", remote_endpoint_.address().to_string(), remote_endpoint_.port());
//
//                        // 1. 에러 응답 송신
//                        nlohmann::json err_resp = {
//                            {"type", "udp_error"},
//                            {"error", "Invalid JSON format"}
//                        };
//                        auto data = std::make_shared<std::string>(err_resp.dump());
//                        socket_.async_send_to(boost::asio::buffer(*data), remote_endpoint_,
//                            [data](const boost::system::error_code& send_ec, std::size_t) {
//                                if (send_ec) {
//                                    AppContext::instance().logger->warn("[UDPManager] Error response send failed: {}", send_ec.message());
//                                }
//                            });
//
//                        // 2. rate limit 증가 및 차단 로직 (간단 예)
//                        if (auto handler2 = data_handler_.lock()) {
//                            handler2->register_bad_udp_packet(remote_endpoint_);
//                            if (handler2->is_blocked_udp(remote_endpoint_)) {
//                                AppContext::instance().logger->warn("[UDPManager] UDP client blocked due to repeated invalid packets: {}:{}", remote_endpoint_.address().to_string(), remote_endpoint_.port());
//                                // 필요하면 이후 패킷 무시 또는 차단 처리
//                            }
//                        }
//                        // 파싱 실패 시 처리 종료
//                        start_receive();
//                        return;
//                    }
//
//                    // 정상 JSON 처리
//                    try {
//                        handler->on_udp_receive(*msg, remote_endpoint_, socket_);
//                    }
//                    catch (const std::exception& e) {
//                        AppContext::instance().logger->error("[UDPManager] on_udp_receive 예외 발생: {}", e.what());
//                    }
//                }
//            }
//            start_receive();
//        });
//}

void UDPManager::close() {
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
        AppContext::instance().logger->error("[UDPManager] udp socket close error : {}", ec.message());
    }
}
