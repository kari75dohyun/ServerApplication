#pragma once
#include "DataHandler.h"
#include "SessionPool.h" 
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <unordered_map>
#include <memory>
#include <atomic>

class SSLServer {
private:
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ssl::context& context_;

    std::atomic<int> session_counter_;
    std::shared_ptr<DataHandler> data_handler_;

public:
    SSLServer(boost::asio::io_context& io, short port, boost::asio::ssl::context& context, std::shared_ptr<DataHandler> data_handler, std::shared_ptr<SessionPool> session_pool);

    void accept();

    std::shared_ptr<SessionPool> session_pool_;
private:
    void start_accept();
};