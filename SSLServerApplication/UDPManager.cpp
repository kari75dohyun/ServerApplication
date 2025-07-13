#include "UDPManager.h"
#include "DataHandler.h"
#include <iostream>
#include "Logger.h"

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
            g_logger->error("[UDPManager] 소멸자에서 udp 소켓 close 에러:  {}", ec.message());
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
                    try {
                        handler->on_udp_receive(*msg, remote_endpoint_, socket_);
                    }
                    catch (const std::exception& e) {
                        g_logger->error("[UDPManager] on_udp_receive 예외 발생: {}", e.what());
                    }
                }
            }
            // 항상 다시 수신 대기
            start_receive();
        });
}

void UDPManager::close() {
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
        g_logger->error("[UDPManager] udp socket close error : {}", ec.message());
    }
}
