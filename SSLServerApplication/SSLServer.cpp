#include "SSLServer.h"
#include "SSLSession.h"
#include "SessionPool.h"
#include <iostream>
#include <boost/asio.hpp>
#include "Logger.h"

using namespace std;
using boost::asio::ip::tcp;
using namespace boost::asio;

SSLServer::SSLServer(boost::asio::io_context& io, short port, ssl::context& context, shared_ptr<DataHandler> data_handler, std::shared_ptr<SessionPool> session_pool)
    : acceptor_(io, tcp::endpoint(tcp::v4(), port)), context_(context), session_counter_(0), data_handler_(data_handler), session_pool_(session_pool) {
    //acceptor_.listen(2048);
    start_accept();
}

void SSLServer::start_accept() {
    acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (!ec) {
            int session_id = session_counter_.fetch_add(1);
            auto session = session_pool_->acquire(std::move(socket), session_id);
            if (session) {
                data_handler_->add_session(session_id, session);
                data_handler_->cleanup_unauth_sessions(100); // 최대 미인증 세션 100개로 제한
                session->start();
                std::cout << "New client connected, session ID: " << session_id << std::endl;
                //LOG_INFO("New client connected, session ID: ", session_id);
                g_logger->info("New client connected, session ID: {}", session_id);
                // IP/포트 바로 출력!
                g_logger->info("New client: session_id={}, IP={}, port={}", session_id, session->get_client_ip(), session->get_client_port());
            }
            else {
                std::cerr << "[SESSION POOL] No free session available!" << std::endl;
                //LOG_ERROR("[SESSION POOL] No free session available!");
                g_logger->info("[SESSION POOL] No free session available!");

                // 연결 닫기 등 예외 처리
            }
        }
        start_accept();
        });
}
