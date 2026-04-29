#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <spdlog/logger.h>
#include <spdlog/sinks/sink.h>

#include "eta/runtime/types/log_types.h"
#include "eta/runtime/vm/vm.h"

namespace eta::log {

using eta::runtime::types::LogFormatterMode;

struct SinkState {
    spdlog::sink_ptr sink;
    bool is_port_sink{false};
    bool is_current_error_sink{false};
};

struct LoggerState {
    std::shared_ptr<spdlog::logger> logger;
    std::string name;
    LogFormatterMode formatter_mode{LogFormatterMode::Human};
    bool has_port_sink{false};
};

struct VmLoggerState {
    std::shared_ptr<LoggerState> default_logger;
};

class GlobalLogState {
public:
    std::shared_ptr<LoggerState> find_named_logger(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = named_loggers_.find(name);
        if (it == named_loggers_.end()) return nullptr;
        auto pinned = it->second.lock();
        if (!pinned) named_loggers_.erase(it);
        return pinned;
    }

    void remember_named_logger(const std::string& name, const std::shared_ptr<LoggerState>& state) {
        std::lock_guard<std::mutex> lk(mu_);
        named_loggers_[name] = state;
    }

    std::shared_ptr<LoggerState> default_logger_for(runtime::vm::VM& vm) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = vm_state_.find(&vm);
        if (it == vm_state_.end()) return nullptr;
        return it->second.default_logger;
    }

    void set_default_logger_for(runtime::vm::VM& vm, const std::shared_ptr<LoggerState>& state) {
        std::lock_guard<std::mutex> lk(mu_);
        vm_state_[&vm].default_logger = state;
    }

    void erase_vm(runtime::vm::VM& vm) {
        std::lock_guard<std::mutex> lk(mu_);
        vm_state_.erase(&vm);
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        named_loggers_.clear();
        vm_state_.clear();
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, std::weak_ptr<LoggerState>> named_loggers_;
    std::unordered_map<runtime::vm::VM*, VmLoggerState> vm_state_;
};

inline GlobalLogState& global_log_state() {
    static GlobalLogState state;
    return state;
}

} ///< namespace eta::log
