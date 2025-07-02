#include "SSLServer.h"
#include "SSLSession.h"
#include <iostream>
#include <boost/asio.hpp>

using namespace std;
using boost::asio::ip::tcp;
using namespace boost::asio;

SSLServer::SSLServer(boost::asio::io_context& io, short port, ssl::context& context, shared_ptr<DataHandler> data_handler)
    : acceptor_(io, tcp::endpoint(tcp::v4(), port)), context_(context), session_counter_(0), data_handler_(data_handler) {
    start_accept();
}

void SSLServer::start_accept() {
    acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (!ec) {
            //int session_id = ++session_counter_;
            int session_id = session_counter_.fetch_add(1);  // atomic 사용
            auto session = make_shared<SSLSession>(std::move(socket), context_, session_id, data_handler_);
            data_handler_->add_session(session_id, session);
            session->start();
            std::cout << "New client connected, session ID: " << session_id << std::endl;
        }
        start_accept();
        });
}

