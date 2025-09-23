#include "DBmwRouter.h"
#include "SessionManager.h"
#include "Session.h"
#include "DataHandler.h"
#include "Logger.h"
#include "AppContext.h"

using namespace std::chrono_literals;

DBmwRouter::DBmwRouter(boost::asio::io_context& io,
    std::shared_ptr<SessionManager> session_manager,
    std::shared_ptr<DataHandler>    data_handler)
    : io_(io),
    strand_(boost::asio::make_strand(io)),
    session_manager_(std::move(session_manager)),
    data_handler_(std::move(data_handler)) {
    // 기본 핸들러
    //handlers_.emplace("login_ok", [this](const json& j) { handle_login_ok(j); });
    //handlers_.emplace("keepalive_ack", [this](const json& j) { handle_keepalive_ack(j); });
    //handlers_.emplace("db_query_result", [this](const json& j) { handle_db_query_result(j); });
    //handlers_.emplace("push_notice", [this](const json& j) { handle_push_notice(j); });
    //handlers_.emplace("error", [this](const json& j) { handle_error_with_req_id(j); });

    //handlers_.emplace("login_ack", [this](const json& j) { handle_login_ack(j); });
}

void DBmwRouter::set_senders(std::function<void(const json&)> send_json_fn,
    std::function<void(const json&)> send_secure_json_fn) {
    send_json_fn_ = std::move(send_json_fn);
    send_secure_json_fn_ = std::move(send_secure_json_fn);
}

void DBmwRouter::register_handler(const std::string& type, Handler h) {
    handlers_[type] = std::move(h);
}

void DBmwRouter::handle(const json& j) {
    const std::string type = j.value("type", "");
    auto it = handlers_.find(type);
    if (it != handlers_.end()) {
        it->second(j);
    }
    else {
        AppContext::instance().logger->info("[DBMW][RX][UNKNOWN] {}", j.dump());
    }
}

void DBmwRouter::handle(const wire::Envelope& env) {
    using T = wire::MsgType;

    switch (env.type()) {
    case T::SERVER_LOGIN_ACK: {
        const auto& ack = env.server_login_ack();
        // JSON 버전과 동일한 처리
        if (ack.result() == "ok") {
            AppContext::instance().logger->info("[DBMW][PB] server_login_ack OK (nickname={})", ack.nickname());
            // 로그인 성공 → 인증 플래그 켜기
            if (auto cli = AppContext::instance().db_client.lock()) {
                cli->mark_authed(true);
            }
        }
        else {
            AppContext::instance().logger->warn("[DBMW][PB] server_login_ack NG (nickname={})", ack.nickname());
        }
        break;
    }
    case T::KEEPALIVE_ACK: {
        // 하트비트 응답 → 미스 카운터 리셋
        if (auto cli = AppContext::instance().db_client.lock()) {
            cli->note_heartbeat_ack();
        }
        break;
    }
    case T::DB_QUERY_RESULT: {
        const auto& r = env.db_query_result();
        // 기존 JSON 경로로 내려주던 구조에 맞춰 변환해 핸들링하거나,
        // 직접 이 자리에서 세션/콜백 매칭 처리해도 됨
        nlohmann::json j{
            {"type","db_query_result"},
            {"api", r.api()},
            {"code", r.code()},
            {"req_id", r.req_id()},
            {"data", r.data_json()} // 서버는 일단 문자열로 받았다가 필요시 파싱
        };
        handle(j); // 기존 JSON 핸들러 재사용
        break;
    }
    case T::ERROR_MSG: {
        const auto& e = env.error();
        nlohmann::json j{
            {"type","db_error"},
            {"code", e.code()},
            {"req_id", e.req_id()},
            {"msg", e.msg()}
        };
        handle(j);
        break;
    }
    default:
        AppContext::instance().logger->warn("[DBMW][PB] unhandled type={}", (int)env.type());
        break;
    }
}


/* =========================
 *   요청/응답 매칭 API
 * ========================= */
std::shared_future<DBmwRouter::json>
DBmwRouter::send_request(const std::string& api,
    json payload,
    bool secure,
    int timeout_ms,
    int max_retries,
    int session_id) {
    // 요청 JSON을 구성
    json req;
    req["type"] = "db_query";
    req["api"] = api;

    // req_id 생성
    const std::string req_id = make_req_id(session_id);
    req["req_id"] = req_id;

    // payload 병합 (payload 키가 req 기본키와 충돌하면 payload 우선으로 해도 됨)
    if (!payload.is_null()) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            req[it.key()] = it.value();
        }
    }

    // strand에서 pending 등록 + 전송 + 타이머
    auto self = shared_from_this();
    std::promise<json> prom;
    auto fut = prom.get_future().share();

    boost::asio::dispatch(strand_, [this, self, req = std::move(req),
        prom = std::move(prom), fut, // fut 캡처
        secure, timeout_ms, max_retries, session_id, req_id]() mutable {

            auto p = std::make_unique<Pending>();
            p->future = fut;               // 재사용
            p->promise = std::move(prom);
            p->timer = std::make_unique<boost::asio::steady_timer>(strand_);
            p->request_json = req;
            p->secure = secure;
            p->retries = 0;
            p->max_retries = max_retries;
            p->timeout_ms = timeout_ms;
            p->session_id = session_id;

            pending_.emplace(req_id, std::move(p));

            // 전송
            send_now_locked(req_id);
            // 타이머
            arm_timeout_locked(req_id);
        });

    return fut;
}

std::string DBmwRouter::send_request_cb(const std::string& api,
    json payload,
    bool secure,
    int timeout_ms,
    int max_retries,
    int session_id,
    ResponseCb on_ok,
    ResponseCb on_error) {
    // 요청 JSON 구성
    json req;
    req["type"] = "db_query";
    req["api"] = api;

    const std::string req_id = make_req_id(session_id);
    req["req_id"] = req_id;

    if (!payload.is_null()) {
        for (auto it = payload.begin(); it != payload.end(); ++it) {
            req[it.key()] = it.value();
        }
    }

    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [this, self, req = std::move(req), secure, timeout_ms, max_retries,
        session_id, req_id, on_ok = std::move(on_ok), on_error = std::move(on_error)]() mutable {
            if ((!secure && !send_json_fn_) || (secure && !send_secure_json_fn_)) {
                // 전송 함수가 설정 안됐으면 즉시 on_error 콜백
                if (on_error) {
                    nlohmann::json err = { {"type","error"},{"code",-1},{"msg","send function not set"},{"req_id",req_id} };
                    on_error(err);
                }
                return;
            }

            auto p = std::make_unique<Pending>();
            p->timer = std::make_unique<boost::asio::steady_timer>(strand_);
            p->request_json = req;
            p->secure = secure;
            p->retries = 0;
            p->max_retries = max_retries;
            p->timeout_ms = timeout_ms;
            p->session_id = session_id;

            p->use_callbacks = true;       // 콜백 모드
            p->on_ok = std::move(on_ok);
            p->on_error = std::move(on_error);

            pending_.emplace(req_id, std::move(p));

            send_now_locked(req_id);
            arm_timeout_locked(req_id);
        });

    return req_id;
}


/* =========================
 *   내부 전송/타임아웃
 * ========================= */
void DBmwRouter::send_now_locked(const std::string& req_id) {
    auto it = pending_.find(req_id);
    if (it == pending_.end()) return;
    auto& p = *(it->second);

    try {
        if (p.secure) {
            if (send_secure_json_fn_) send_secure_json_fn_(p.request_json);
        }
        else {
            if (send_json_fn_) send_json_fn_(p.request_json);
        }
        m_sent_.fetch_add(1);
        AppContext::instance().logger->info("[DBMW][TX] api={}, req_id={}, secure={}, retry={}/{}",
            p.request_json.value("api", ""), req_id, p.secure, p.retries, p.max_retries);
    }
    catch (const std::exception& e) {
        AppContext::instance().logger->error("[DBMW][TX] exception: {}", e.what());
    }
}

void DBmwRouter::arm_timeout_locked(const std::string& req_id) {
    auto it = pending_.find(req_id);
    if (it == pending_.end()) return;
    auto& p = *(it->second);

    p.timer->expires_after(std::chrono::milliseconds(p.timeout_ms));
    auto self = shared_from_this();
    p.timer->async_wait([this, self, req_id](const boost::system::error_code& ec) {
        if (ec) return; // cancel됨
        // strand로 재진입
        boost::asio::dispatch(strand_, [this, self, req_id] {
            auto it2 = pending_.find(req_id);
            if (it2 == pending_.end()) return;
            auto& pp = *(it2->second);

            if (pp.retries < pp.max_retries) {
                ++pp.retries;
                m_retry_.fetch_add(1);
                AppContext::instance().logger->warn("[DBMW][TO] retry req_id={} ({}/{})", req_id, pp.retries, pp.max_retries);
                send_now_locked(req_id);
                arm_timeout_locked(req_id);
            }
            else {
                m_to_.fetch_add(1);
                json err = {
                    {"type","error"},
                    {"code",-110},                // timeout code (예시)
                    {"msg","timeout"},
                    {"req_id",req_id}
                };
                fulfill_and_erase_locked(req_id, err, /*success=*/false);
            }
            });
        });
}

void DBmwRouter::fulfill_and_erase_locked(const std::string& req_id,
    const json& result, bool success) {
    auto it = pending_.find(req_id);
    if (it == pending_.end()) return;

    auto p = std::move(it->second);

    // 1) 타이머 취소 (인자 없는 cancel())
    try { p->timer->cancel(); }
    catch (...) {}

    // 2) 테이블에서 제거
    pending_.erase(it);

    // 3) promise fulfill
    try { p->promise.set_value(result); }
    catch (...) {}

    if (success) m_ok_.fetch_add(1);
    else         m_fail_.fetch_add(1);
}

/* =========================
 *   라우팅 핸들러
 * ========================= */
//void DBmwRouter::handle_login_ack(const json& j) {
//    const std::string result = j.value("result", "");
//    const std::string nickname = j.value("nickname",
//        AppContext::instance().config.value("dbmw_login_nickname", std::string("")));
//
//    if (result == "ok") {
//        AppContext::instance().logger->info("[DBMW][RX] login_ack OK (nickname={})", nickname);
//
//        // (선택) 서버 클라에게 공지
//        if (data_handler_) {
//            nlohmann::json out = {
//                {"type","notice"},
//                {"msg", "DB middleware authenticated"}
//            };
//            data_handler_->broadcast_strict(out.dump());
//        }
//
//        // (선택) 프로브 요청 한번 보내서 왕복 확인하고 싶으면 주석 해제:
//        // try {
//        //     auto fut = send_request("echo",
//        //         json{{"msg","hello-from-game-server"}},
//        //         /*secure=*/true, /*timeout_ms=*/2000, /*max_retries=*/1, /*session_id=*/-1);
//        //     std::thread([fut]() mutable {
//        //         try { auto r = fut.get();
//        //               AppContext::instance().logger->info("[DBMW][PROBE] echo result: {}", r.dump());
//        //         } catch (const std::exception& e) {
//        //               AppContext::instance().logger->error("[DBMW][PROBE] exception: {}", e.what());
//        //         }
//        //     }).detach();
//        // } catch (const std::exception& e) {
//        //     AppContext::instance().logger->error("[DBMW][PROBE] start failed: {}", e.what());
//        // }
//    }
//    else {
//        // 실패인 경우도 로그
//        AppContext::instance().logger->warn("[DBMW][RX] login_ack NG: {}", j.dump());
//    }
//}
//void DBmwRouter::handle_login_ok(const json& j) {
//    AppContext::instance().logger->info("[DBMW][RX] login_ok");
//}
//void DBmwRouter::handle_keepalive_ack(const json& /*j*/) {
//    // 필요 시 로그
//}
//
//void DBmwRouter::handle_db_query_result(const json& j) {
//    const std::string req_id = j.value("req_id", "");
//    if (req_id.empty()) {
//        AppContext::instance().logger->warn("[DBMW][RX] db_query_result without req_id");
//        return;
//    }
//
//    auto self = shared_from_this();
//    boost::asio::dispatch(strand_, [this, self, req_id, j] {
//        auto it = pending_.find(req_id);
//        if (it == pending_.end()) {
//            AppContext::instance().logger->info("[DBMW][RX] late/unknown req_id: {}", req_id);
//            return;
//        }
//
//        // 콜백 모드면 콜백 호출, 아니면 기존 future fulfill
//        if (it->second->use_callbacks && it->second->on_ok) {
//            auto cb = std::move(it->second->on_ok);
//            fulfill_and_erase_locked(req_id, j, /*success=*/true); // 내부에서 타이머 cancel/erase
//            cb(j);
//            return;
//        }
//
//        fulfill_and_erase_locked(req_id, j, /*success=*/true);
//
//        // (legacy) future 기반: 세션으로 포워딩 로직 유지하고 싶으면 아래 계속 사용
//        int sid = extract_session_id_from_req(req_id);
//        if (sid >= 0 && session_manager_) {
//            if (auto sess = session_manager_->find_session(sid)) {
//                nlohmann::json to_client = {
//                    {"type","db_query_result"},
//                    {"api", j.value("api","")},
//                    {"code", j.value("code",0)},
//                    {"data", j.value("data", json::object())}
//                };
//                sess->post_write(to_client.dump());
//            }
//        }
//        });
//}
//
//void DBmwRouter::handle_push_notice(const json& j) {
//    nlohmann::json out = {
//        {"type","notice"},
//        {"msg", j.value("msg","")}
//    };
//    if (data_handler_) data_handler_->broadcast_strict(out.dump());
//}
//
//void DBmwRouter::handle_error_with_req_id(const json& j) {
//    const std::string req_id = j.value("req_id", "");
//    if (req_id.empty()) {
//        AppContext::instance().logger->warn("[DBMW][RX] error without req_id: {}", j.dump());
//        return;
//    }
//    auto self = shared_from_this();
//    boost::asio::dispatch(strand_, [this, self, req_id, j] {
//        auto it = pending_.find(req_id);
//        if (it == pending_.end()) return;
//
//        if (it->second->use_callbacks && it->second->on_error) {
//            auto cb = std::move(it->second->on_error);
//            fulfill_and_erase_locked(req_id, j, /*success=*/false);
//            cb(j);
//            return;
//        }
//
//        fulfill_and_erase_locked(req_id, j, /*success=*/false);
//        });
//}

/* =========================
 *   유틸/메트릭
 * ========================= */

int DBmwRouter::extract_session_id_from_req(const std::string& req_id) {
    // 예: "123_login", "123-anything", "srv-42-00000123"
    // 단순 숫자 prefix 파싱
    std::string digits;
    for (char c : req_id) {
        if (c >= '0' && c <= '9') digits.push_back(c);
        else break;
    }
    if (digits.empty()) return -1;
    try { return std::stoi(digits); }
    catch (...) { return -1; }
}

std::string DBmwRouter::make_req_id(int session_id) {
    // 예: "srv-<pid>-<seq>" 또는 "123_<seq>"
    uint64_t seq = ++req_seq_;
    if (session_id >= 0) {
        return std::to_string(session_id) + "_" + std::to_string(seq);
    }
#ifdef _WIN32
    int pid = _getpid();
#else
    int pid = getpid();
#endif
    return std::string("srv-") + std::to_string(pid) + "-" + std::to_string(seq);
}

DBmwRouter::Metrics DBmwRouter::metrics_snapshot() const {
    Metrics m;
    m.sent = m_sent_.load();
    m.ok = m_ok_.load();
    m.timed_out = m_to_.load();
    m.retried = m_retry_.load();
    m.failed = m_fail_.load();
    return m;
}

void DBmwRouter::log_metrics() const {
    auto m = metrics_snapshot();
    AppContext::instance().logger->info(
        "[DBMW][Metrics] sent={}, ok={}, timeout={}, retry={}, failed={}",
        m.sent, m.ok, m.timed_out, m.retried, m.failed);
}
