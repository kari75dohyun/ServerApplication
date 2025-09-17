#include "Utility.h"
#include "AppContext.h"
#include "Logger.h"

std::optional<nlohmann::json> try_parse_json(const std::string& msg) {
    try {
        return nlohmann::json::parse(msg);
    }
    catch (const std::exception& e) {
        // ���� .cpp �̹Ƿ� AppContext �����ϰ� ��� ����
        if (AppContext::instance().logger) {
            AppContext::instance().logger->info(
                "[UDP] JSON Parse failed: {} / original: {}", e.what(), msg);
        }
        return std::nullopt;
    }
}
