#include "DataHandler.h" 
#include "SSLSession.h"
#include <boost/asio.hpp>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;
using namespace boost::asio;
using boost::asio::ip::udp;

DataHandler::DataHandler()
    : shard_count(std::max(4u, std::thread::hardware_concurrency() * 2)),
    session_buckets(shard_count),
    session_mutexes(shard_count), dispatcher_(this) {
}

void DataHandler::add_session(int session_id, std::shared_ptr<SSLSession> session) {
    int shard = get_shard(session_id);
    std::lock_guard<std::mutex> lock(session_mutexes[shard]);

    auto it = session_buckets[shard].find(session_id);
    if (it != session_buckets[shard].end()) {
        std::cerr << "[add_session] FATAL: session_id " << session_id << " already exists! Closing previous session." << std::endl;
        if (it->second) it->second->close_session();
        session_buckets[shard].erase(it);
    }

    session_buckets[shard][session_id] = session;
    std::cout << "[add_session] session_id=" << session_id << " added to shard " << shard << std::endl;
}

void DataHandler::remove_session(int session_id) {
    int shard = get_shard(session_id);
    std::lock_guard<std::mutex> lock(session_mutexes[shard]);
    try {
        auto it = session_buckets[shard].find(session_id);
        if (it != session_buckets[shard].end()) {
            session_buckets[shard].erase(it);
            std::cout << "[remove_session] session_id=" << session_id << " removed from shard " << shard << std::endl;
        }
        else {
            std::cout << "[remove_session] session_id=" << session_id << " not found (already removed?)" << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[remove_session] Exception: " << e.what() << std::endl;
    }
}

shared_ptr<SSLSession> DataHandler::get_session(int session_id) {
    int shard = get_shard(session_id);
    std::lock_guard<std::mutex> lock(session_mutexes[shard]);
    auto it = session_buckets[shard].find(session_id);
    if (it != session_buckets[shard].end())
        return it->second;
    return nullptr;
}

void DataHandler::do_handshake(std::shared_ptr<SSLSession> session) {
    session->post_task([this, session]() {
        auto self = session;
        auto& strand = session->get_strand();
        session->get_socket().async_handshake(boost::asio::ssl::stream_base::server,
            boost::asio::bind_executor(strand, [this, self](const boost::system::error_code& ec) {
                if (!ec) {
                    self->set_message(R"({"type":"notice","msg":"Enter your nickname:"})" "\n");
                    //do_write(self);
                    self->post_write(self->get_message());

                    do_read(self); // 최초 read 예약! (핸드쉐이크 성공 직후)
                }
                else {
                    std::cerr << "Handshake failed: " << ec.message() << std::endl;
                }
                self->run_next_task();
                })
        );
        });
}

void DataHandler::do_read(shared_ptr<SSLSession> session) {
    // [1] 중복 read 방지!
    if (!session->try_acquire_read()) {
        std::cerr << "[WARN] 중복 do_read 감지! session_id=" << session->get_session_id() << std::endl;
        return;
    }
    // do_read를 직접 호출하지 않고, post_task로 감싼다.
    session->post_task([this, session]() {
        auto self = session;
        auto& strand = session->get_strand();
        session->get_socket().async_read_some(
            buffer(session->get_data(), sizeof(session->get_data())),
            boost::asio::bind_executor(strand, [this, self](const boost::system::error_code& ec, std::size_t length) {
                // [2] 콜백 진입 시 반드시 해제!
                self->release_read();

                try {
                    if (!ec) {
                        // 1. 누적 버퍼에 append
                        self->get_msg_buffer().append(self->get_data(), length);

                        // 2. 여러 메시지 추출 및 처리
                        while (auto opt_msg = self->get_msg_buffer().extract_message()) {
                            try {
                                json msg = json::parse(*opt_msg);
                                dispatcher_.dispatch(self, msg);
                            }
                            catch (const std::exception& e) {
                                std::cerr << "[JSON parsing error] " << e.what() << " / data: " << *opt_msg << std::endl;
                                self->set_message(R"({"type":"error","msg":"Message parsing failed"})" "\n");
                                //do_write(self);
                                self->post_write(self->get_message());
                                // 에러 시에도 계속 다음 메시지 분리/처리
                            }
                        }

                        // 3. 계속해서 read (이 구조면 wrote 체크 필요 없음)
                        do_read(self);
                    }
                    else if (ec == boost::asio::error::eof) {
                        std::cout << "Client disconnected." << std::endl;
                        // 퇴장 알림
                        std::string nickname = self->get_nickname();
                        json notice;
                        notice["type"] = "notice";
                        notice["msg"] = nickname + " has left.";
                        broadcast(notice.dump() + "\n", self->get_session_id(), self);

                        self->close_session();  // 세션 종료
                    }
                    else if (ec == boost::asio::error::connection_reset) {
                        std::cout << "Client forcibly disconnected." << std::endl;
                        std::string nickname = self->get_nickname();
                        if (nickname.empty()) {
                            std::cerr << "Client disconnected without a nickname." << std::endl;
                        }
                        else {
                            json notice;
                            notice["type"] = "notice";
                            notice["msg"] = nickname + " has left.";
                            broadcast(notice.dump() + "\n", self->get_session_id(), self);
                            std::cout << "Client with nickname '" << nickname << "' disconnected." << std::endl;
                        }

                        self->close_session();
                    }
                    else {
                        std::cerr << "Read failed: [" << ec.value() << "] " << ec.message() << std::endl;
                        self->close_session();
                    }
                }
                catch (const std::exception& e) {
                    std::cerr << "[FATAL][do_read handler 예외] " << e.what() << std::endl;
                    self->close_session();
                }
                self->run_next_task(); // 항상 마지막에!

                })
            );
    });
}

//void DataHandler::do_write(shared_ptr<SSLSession> session) {
//    auto self = session;
//    auto& strand = session->get_strand();
//    async_write(session->get_socket(), buffer(self->get_message()),
//        boost::asio::bind_executor(strand, [this, self](const boost::system::error_code& ec, std::size_t length) {
//            if (!ec) {
//                std::cout << "Sent: " << self->get_message() << std::endl;
//                //do_read(self);  // do_read도 마찬가지(아래 주의)
//            }
//            else {
//                std::cerr << "Write failed: " << ec.message() << std::endl;
//                self->close_session();
//            }
//            self->run_next_task(); // 직렬화 큐 구조
//            })
//    );
//}

void DataHandler::broadcast(const std::string& msg, int sender_session_id, std::shared_ptr<SSLSession> /*session*/) {
    std::vector<std::shared_ptr<SSLSession>> targets;

    // 1. 각 샤드별로 lock (세션 포인터만 복사)
    for (unsigned int shard = 0; shard < shard_count; ++shard) {
        std::lock_guard<std::mutex> lock(session_mutexes[shard]);
        for (const auto& [id, sess] : session_buckets[shard]) {
            if (sess && id != sender_session_id) {
                targets.push_back(sess);
            }
        }
    }

    // 2. 락 해제 후 각 세션에 post_write로 write 작업 직렬화
    for (auto& sess : targets) {
        try {
            std::cout << "[broadcast] target session_id=" << sess->get_session_id()
                << " (sender_session_id=" << sender_session_id << ")" << std::endl;

            sess->post_write(msg);  // <--- write 직렬화 큐 진입점
        }
        catch (const std::exception& e) {
            std::cerr << "[broadcast] Exception scheduling broadcast: " << e.what() << std::endl;
        }
    }
}


//void DataHandler::broadcast(const std::string& msg, int sender_session_id, std::shared_ptr<SSLSession> /*session*/) {
//    std::vector<std::shared_ptr<SSLSession>> targets;
//
//    // 1. 각 샤드별로 lock (세션 포인터만 복사)
//    for (unsigned int shard = 0; shard < shard_count; ++shard) {
//        std::lock_guard<std::mutex> lock(session_mutexes[shard]);
//        for (const auto& [id, sess] : session_buckets[shard]) {
//            // 반드시 본인 세션은 브로드캐스트 대상에서 제외
//            if (sess && id != sender_session_id) {
//                targets.push_back(sess);
//            }
//        }
//    }
//
//    // 2. 락 해제 후 각 세션에 post_task로 write 작업 직렬화
//    for (auto& sess : targets) {
//        try {
//            // 로그 남기기
//            std::cout << "[broadcast] target session_id=" << sess->get_session_id()
//                << " (sender_session_id=" << sender_session_id << ")" << std::endl;
//
//            sess->post_task([this, sess, msg]() {
//                sess->set_message(msg);
//                async_write(sess->get_socket(), buffer(sess->get_message()),
//                    boost::asio::bind_executor(sess->get_strand(),
//                        [this, sess](const boost::system::error_code& ec, std::size_t /*length*/) {
//                            if (ec) {
//                                std::cerr << "[broadcast] Write failed: " << ec.message() << std::endl;
//                                sess->close_session();
//                            }
//                            sess->run_next_task(); // 꼭 마지막에!
//                        }
//                    )
//                );
//                });
//        }
//        catch (const std::exception& e) {
//            std::cerr << "[broadcast] Exception scheduling broadcast: " << e.what() << std::endl;
//        }
//    }
//}



// UDP 메시지 수신 처리
void DataHandler::on_udp_receive(const std::string& msg, const udp::endpoint& from, udp::socket& udp_socket) {
    try {
        // 1. JSON 파싱 시도
        json jmsg = json::parse(msg);

        std::string type = jmsg.value("type", "");
        std::string nickname = jmsg.value("nickname", "anonymity");
        std::string udp_data = jmsg.value("msg", "");

        // 2. 로그 출력 (실제 활용에 따라 변경 가능)
        std::cout << "[UDP] From: " << from.address().to_string() << ":" << from.port()
            << " | Nickname: " << nickname << " | Message: " << udp_data << std::endl;

        // 3. (선택) UDP 메시지 타입에 따라 별도 분기 가능
        if (type == "udp") {
            // 단순 Echo (닉네임 포함 응답)
            json response;
            response["type"] = "udp_reply";
            response["msg"] = "Echo(UDP): " + udp_data;
            response["nickname"] = nickname;

            std::string send_data = response.dump();

            udp_socket.async_send_to(
                boost::asio::buffer(send_data), from,
                [](const boost::system::error_code& ec, std::size_t) {
                    if (ec) {
                        std::cerr << "[UDP] Send error: " << ec.message() << std::endl;
                    }
                }
            );
        }
        else {
            // 알 수 없는 타입일 때 기본 Echo
            std::string response = "Echo(UDP): " + msg;
            udp_socket.async_send_to(
                boost::asio::buffer(response), from,
                [](const boost::system::error_code& ec, std::size_t) {
                    if (ec) {
                        std::cerr << "[UDP] Send error: " << ec.message() << std::endl;
                    }
                }
            );
        }
    }
    catch (const std::exception& e) {
        // JSON 파싱 실패 시, 원래 메시지 그대로 에코
        std::cerr << "[UDP] JSON Parse failed: " << e.what() << " / original: " << msg << std::endl;
        std::string response = "Echo(UDP): " + msg;
        udp_socket.async_send_to(
            boost::asio::buffer(response), from,
            [](const boost::system::error_code& ec, std::size_t) {
                if (ec) {
                    std::cerr << "[UDP] Send error: " << ec.message() << std::endl;
                }
            }
        );
    }
}

// 전체 세션을 "정확하게" 순회 (락 순서대로 걸고 해제)
void DataHandler::for_each_session(std::function<void(const std::shared_ptr<SSLSession>&)> fn) {
    // 1. 모든 샤드 lock (락 순서 꼭 지켜야 deadlock 방지)
    for (unsigned int i = 0; i < shard_count; ++i)
        session_mutexes[i].lock();

    // 2. 모든 세션에 대해 콜백 호출
    for (unsigned int i = 0; i < shard_count; ++i) {
        for (const auto& [id, sess] : session_buckets[i]) {
            fn(sess);
        }
    }

    // 3. lock 해제 (순서 무관, but 관례적으로 0~N순서)
    for (unsigned int i = 0; i < shard_count; ++i)
        session_mutexes[i].unlock();
}

// 전체 세션 수 정확히 세기
size_t DataHandler::get_total_session_count() {
    size_t total = 0;
    for (unsigned int i = 0; i < shard_count; ++i)
        session_mutexes[i].lock();
    for (unsigned int i = 0; i < shard_count; ++i)
        total += session_buckets[i].size();
    for (unsigned int i = 0; i < shard_count; ++i)
        session_mutexes[i].unlock();
    return total;
}

// (샤드 전체 락 + 일관된 순회)
template<typename Func>
void DataHandler::for_each_session(Func&& func) {
    // 1. 모든 샤드 lock (deadlock 방지: 항상 0~N순)
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(shard_count);
    for (unsigned int shard = 0; shard < shard_count; ++shard) {
        locks.emplace_back(session_mutexes[shard]);
    }

    // 2. 전체 세션 순회
    for (unsigned int shard = 0; shard < shard_count; ++shard) {
        for (const auto& [id, sess] : session_buckets[shard]) {
            func(sess);
        }
    }
    // 3. lock 자동 해제(스코프 종료)
}

// DataHandler.cpp 또는 외부에서
// 긴급 서버공지, 전원강제알림 등 필요할 때
void DataHandler::broadcast_strict(const std::string& msg) {
    for_each_session([msg, this](const std::shared_ptr<SSLSession>& sess) {
        if (sess) sess->post_write(msg);
        });
}