#include "SSLServer.h"
#include "SSLSession.h"
#include "UDPManager.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include "Logger.h"
//#include <windows.h>

using namespace std;
using boost::asio::ip::tcp;
using namespace boost::asio;

constexpr size_t POOL_SIZE = 1024; // 원하는 값으로!
constexpr size_t MAX_POOL_SIZE = 10000;  // session 풀을 1024개가 넘으면 자동 증가 하지만, max 사이즈 만큼은 못넘게 한다.

int main() {
    init_logger();

    auto logger = spdlog::get("server");

    g_logger->info("=== 서버 시작! ===");

    try {
        // 1. io_context 준비
        boost::asio::io_context io;

        // 2. SSL context, 기타 준비
        ssl::context context(ssl::context::tlsv12);
        context.use_certificate_chain_file("server.pem");
        context.use_private_key_file("server.key", ssl::context::pem);

        // 3. DataHandler 인스턴스 생성 (io를 전달)
        // DataHandler 객체 생성 및 공유 포인터로 관리
        auto session_manager = std::make_shared<SessionManager>(max(4u, thread::hardware_concurrency() * 2));

        auto data_handler = std::make_shared<DataHandler>(io, session_manager);
        //auto data_handler = std::make_shared<DataHandler>(io);

        // 4. 세션풀, 서버 등 생성
        auto session_pool = std::make_shared<SessionPool>(POOL_SIZE, MAX_POOL_SIZE, io, context, data_handler);
        SSLServer server(io, 12345, context, data_handler, session_pool);

        // 5. === 여기에서 글로벌 keepalive 타이머 루프 시작 ===
        //data_handler->start_keepalive_loop();  // 클라가 하트비트 보내는 구조로 변경됨 DataHandler 생성자에서 호출해버림

        // 6. UDP 등 기타 서버 준비
        UDPManager udp_manager(io, 54321, data_handler); // UDP 매니저 생성

        cout << "SSL Echo Server started on port 12345" << endl;
        //LOG_INFO("SSL Echo Server started on port 12345");
        g_logger->info("SSL Echo Server started on port 12345");

        // 7. 스레드 풀 및 io.run()
        size_t thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        cout << "Thread count: " << thread_count << endl;
        //LOG_INFO("Thread count: ", thread_count);
        g_logger->info("Thread count: {}", thread_count);

        vector<thread> threads;
        for (size_t i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io]() {
                try {
                    io.run();
                }
                catch (const std::exception& e) {
                    //std::cerr << "[FATAL] io_context.run()에서 예외 발생: " << e.what() << std::endl;
                    //LOG_ERROR("[FATAL] io_context.run()에서 예외 발생: ", e.what());
                    g_logger->error("[FATAL] io_context.run()에서 예외 발생: {}", e.what());
                    // 로그 남기고, 필요하다면 복구 시도
                }
                });
        }

        for (auto& t : threads)
            t.join();

    }
    catch (const std::exception& e) {
        cerr << "Exception: " << e.what() << endl;
        //LOG_ERROR("Exception: ", e.what());
        g_logger->error("Exception: {}", e.what());
    }

    return 0;
}