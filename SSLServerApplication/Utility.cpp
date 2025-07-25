#include "SSLSession.h"
#include "Utility.h"
#include <iostream>
#include <curl/curl.h>
#include "Logger.h"

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
        g_logger->error("[send_admin_alert] Slack 전송 실패: {}", curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
}

bool check_user_udp_rate_limit(SSLSession& sess, size_t user_limit) {
    auto now = std::chrono::steady_clock::now();
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (now - sess.get_udp_packet_window() > std::chrono::seconds(1)) {
        sess.set_udp_packet_window(now);
        sess.set_udp_packet_count(0);
    }
    sess.inc_udp_packet_count();
    return sess.get_udp_packet_count() <= user_limit;
}