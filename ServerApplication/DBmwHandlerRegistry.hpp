#pragma once
#include "DBmwHandlerRegistry.h"
#include "DBmwRouter.h"
#include <nlohmann/json.hpp>

// 사용법: REGISTER_DBMW_HANDLER("login_ack", OnLoginAck)
// void OnLoginAck(DBmwRouter& router, const nlohmann::json& j)

#define REGISTER_DBMW_HANDLER(TYPE, FN)                                       \
namespace {                                                                   \
struct _DBmwReg_##FN {                                                        \
    _DBmwReg_##FN() {                                                         \
        DBmwHandlerRegistry::instance().add(                                  \
            [](DBmwRouter& r){                                                \
                r.register_handler((TYPE), [&r](const nlohmann::json& j){     \
                    FN(r, j);                                                 \
                });                                                           \
            }                                                                 \
        );                                                                    \
    }                                                                         \
};                                                                            \
static _DBmwReg_##FN _dbmw_reg_instance_##FN;                                 \
}
