#include "Session.h"
#include "Utility.h"
#include <iostream>
#include <curl/curl.h>
#include "Logger.h"
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include "AppContext.h"
#include <fstream>
//nlohmann::json g_config;

void send_admin_alert(const std::string& message) {
    static const char* slack_webhook_url = "https://hooks.slack.com/services/XXX/YYY/ZZZ"; // 본인 슬랙 URL로 교체
    CURL* curl = curl_easy_init();
    if (!curl) return;

    std::string payload = R"({"text":")" + message + R"("})";
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, slack_webhook_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        // 실패 시 로컬 로그도 남김
        AppContext::instance().logger->error("[send_admin_alert] Slack 전송 실패: {}", curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
}

//bool check_user_udp_rate_limit(Session& sess, size_t user_limit) {
//    auto now = std::chrono::steady_clock::now();
//    static std::mutex m;
//    std::lock_guard<std::mutex> lock(m);
//    if (now - sess.get_udp_packet_window() > std::chrono::seconds(1)) {
//        sess.set_udp_packet_window(now);
//        sess.set_udp_packet_count(0);
//    }
//    sess.inc_udp_packet_count();
//    return sess.get_udp_packet_count() <= user_limit;
//}

std::string get_env_secret(const std::string& env_name) {
#ifdef _WIN32
    char* val = nullptr;
    size_t len = 0;
    errno_t err = _dupenv_s(&val, &len, env_name.c_str());
    if (err || val == nullptr) return "";
    std::string result(val);
    free(val);
    return result;
#else
    const char* val = getenv(env_name.c_str());
    if (!val) return "";
    return std::string(val);
#endif
}

void load_config(const std::string& filename) {
    std::ifstream in(filename);

    if (!in.is_open()) {
        AppContext::instance().config = nlohmann::json::object(); // fallback
        throw std::runtime_error("config 파일 오픈 실패: " + filename);
    }
    else {
        AppContext::instance().config = nlohmann::json::parse(in);
    }
}