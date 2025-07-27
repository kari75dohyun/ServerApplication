#pragma once
#include "DataHandler.h"
#include "SessionPool.h" 
#include <boost/asio.hpp>
#include <unordered_map>
#include <memory>
#include <atomic>

class Server {
private:
    boost::asio::ip::tcp::acceptor acceptor_;

    std::atomic<int> session_counter_;
    std::shared_ptr<DataHandler> data_handler_;

public:
    Server(boost::asio::io_context& io, short port, std::shared_ptr<DataHandler> data_handler, std::shared_ptr<SessionPool> session_pool);

    void accept();

    std::shared_ptr<SessionPool> session_pool_;
private:
    void start_accept();
};