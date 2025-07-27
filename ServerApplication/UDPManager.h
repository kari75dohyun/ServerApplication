#pragma once

#include <boost/asio.hpp>
#include <memory>

class DataHandler; // 전방 선언

class UDPManager {
public:
    UDPManager(boost::asio::io_context& io, unsigned short port, std::shared_ptr<DataHandler> data_handler);
    ~UDPManager();

    void start_receive();
    void close();

private:
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint remote_endpoint_;
    std::array<char, 1024> buffer_;
    std::weak_ptr<DataHandler> data_handler_; // 순환참조 방지
};
