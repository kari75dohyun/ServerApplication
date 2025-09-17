#include "Utility.h"
#include "AppContext.h"
#include "Logger.h"

std::optional<nlohmann::json> try_parse_json(const std::string& msg) {
    try {
        return nlohmann::json::parse(msg);
    }
    catch (const std::exception& e) {
        // 이제 .cpp 이므로 AppContext 안전하게 사용 가능
        if (AppContext::instance().logger) {
            AppContext::instance().logger->info(
                "[UDP] JSON Parse failed: {} / original: {}", e.what(), msg);
        }
        return std::nullopt;
    }
}
