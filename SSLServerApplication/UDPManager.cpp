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
                // 안전하게 string을 생성
                auto msg = std::make_shared<std::string>(buffer_.data(), bytes_recvd);

                if (auto handler = data_handler_.lock()) {
                    // Echo 응답 문자열을 미리 생성
                    auto response = std::make_shared<std::string>("Echo(UDP): " + *msg);

                    std::cout << "[UDP] Received from " << remote_endpoint_.address().to_string()
                        << ":" << remote_endpoint_.port()
                        << " - " << *msg << std::endl;

                    socket_.async_send_to(
                        boost::asio::buffer(*response), remote_endpoint_,
                        [response](const boost::system::error_code& send_ec, std::size_t) {
                            if (send_ec) {
                                std::cerr << "[UDP] Send error: " << send_ec.message() << std::endl;
                            }
                        });
                }
            }
            // 항상 다시 수신 대기
            start_receive();
        });
}
