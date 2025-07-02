#pragma once
#include "DataHandler.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <unordered_map>
#include <memory>
#include <atomic>

class SSLServer {
private:
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ssl::context& context_;
    //int session_counter_;
    std::atomic<int> session_counter_;   // atomic으로 변경
    std::shared_ptr<DataHandler> data_handler_;

public:
    SSLServer(boost::asio::io_context& io, short port, boost::asio::ssl::context& context, std::shared_ptr<DataHandler> data_handler);

    void accept();

private:
    void start_accept();
};