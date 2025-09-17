#pragma once
#include <functional>
#include <vector>

class DBmwRouter;

class DBmwHandlerRegistry {
public:
    using Registrar = std::function<void(DBmwRouter&)>;

    static DBmwHandlerRegistry& instance() {
        static DBmwHandlerRegistry inst;
        return inst;
    }

    void add(Registrar r) { regs_.push_back(std::move(r)); }
    void attach(DBmwRouter& router) {
        for (auto& r : regs_) r(router);
    }

private:
    std::vector<Registrar> regs_;
};