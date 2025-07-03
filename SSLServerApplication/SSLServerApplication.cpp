// ServerApplication.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include "SSLServer.h"
#include "SSLSession.h"
#include "UDPManager.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
//#include <windows.h>


using namespace std;
using boost::asio::ip::tcp;
using namespace boost::asio;

constexpr size_t POOL_SIZE = 1024; // 원하는 값으로!

int main() {
    SetConsoleOutputCP(CP_UTF8);  // 출력
    SetConsoleCP(CP_UTF8);        // 입력
    setlocale(LC_ALL, "");        // 로케일 설정
    try {
        boost::asio::io_context io;
        ssl::context context(ssl::context::tlsv12);

        context.use_certificate_chain_file("D://Study//Boost_MulitThread_Strand_Parallel_SSL//x64//Debug//server.pem");
        context.use_private_key_file("D://Study//Boost_MulitThread_Strand_Parallel_SSL//x64//Debug//server.key", ssl::context::pem);

        // DataHandler 객체 생성 및 공유 포인터로 관리
        auto data_handler = std::make_shared<DataHandler>();
        auto session_pool = std::make_shared<SessionPool>(POOL_SIZE, io, context, data_handler);

        SSLServer server(io, 12345, context, data_handler, session_pool);


		UDPManager udp_manager(io, 54321, data_handler); // UDP 매니저 생성
        //auto udp_server = std::make_shared<UDPManager>(io, 54321); // 예: 54321 포트

        cout << "SSL Echo Server started on port 12345" << endl;

        size_t thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) thread_count = 4;
        cout << "Thread count: " << thread_count << endl;

        vector<thread> threads;
        for (size_t i = 0; i < thread_count; ++i) {
            threads.emplace_back([&io]() {
                try {
                    io.run();
                }
                catch (const std::exception& e) {
                    std::cerr << "[FATAL] io_context.run()에서 예외 발생: " << e.what() << std::endl;
                    // 로그 남기고, 필요하다면 복구 시도
                }
                });
        }

        for (auto& t : threads)
            t.join();
    }
    catch (const std::exception& e) {
        cerr << "Exception: " << e.what() << endl;
    }

    return 0;
}


// 프로그램 실행: <Ctrl+F5> 또는 [디버그] > [디버깅하지 않고 시작] 메뉴
// 프로그램 디버그: <F5> 키 또는 [디버그] > [디버깅 시작] 메뉴

// 시작을 위한 팁: 
//   1. [솔루션 탐색기] 창을 사용하여 파일을 추가/관리합니다.
//   2. [팀 탐색기] 창을 사용하여 소스 제어에 연결합니다.
//   3. [출력] 창을 사용하여 빌드 출력 및 기타 메시지를 확인합니다.
//   4. [오류 목록] 창을 사용하여 오류를 봅니다.
//   5. [프로젝트] > [새 항목 추가]로 이동하여 새 코드 파일을 만들거나, [프로젝트] > [기존 항목 추가]로 이동하여 기존 코드 파일을 프로젝트에 추가합니다.
//   6. 나중에 이 프로젝트를 다시 열려면 [파일] > [열기] > [프로젝트]로 이동하고 .sln 파일을 선택합니다.
