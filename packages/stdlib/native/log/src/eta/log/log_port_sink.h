#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/sinks/base_sink.h>

#include "eta/runtime/memory/heap.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/vm/vm.h"

namespace eta::log {

/**
 * @brief Synchronous spdlog sink that writes through Eta Port objects.
 *
 * This sink preserves Eta's port-redirection semantics. When configured as a
 * current-error sink it resolves `vm.current_error_port()` at emit-time.
 *
 * Named loggers live in spdlog's process-wide registry, so a sink can outlive
 * its heap and VM.  It pins its fixed port (the GC cannot see it otherwise)
 * and checks the heap and VM liveness tokens before touching either.
 */
class EtaPortSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    EtaPortSink(runtime::memory::heap::Heap& heap,
                runtime::vm::VM* vm,
                runtime::nanbox::LispVal fixed_port,
                bool current_error_sink)
        : heap_(heap),
          heap_alive_(heap.alive_token()),
          vm_(vm),
          vm_alive_(vm ? vm->alive_token() : nullptr),
          fixed_port_(fixed_port),
          current_error_sink_(current_error_sink) {
        heap_.pin(fixed_port_);
    }

    ~EtaPortSink() override {
        if (heap_is_alive()) heap_.unpin(fixed_port_);
    }

    EtaPortSink(const EtaPortSink&) = delete;
    EtaPortSink& operator=(const EtaPortSink&) = delete;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        auto* port_obj = resolve_output_port();
        if (!port_obj) return;

        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);
        auto write_res = port_obj->port->write_string(std::string(formatted.data(), formatted.size()));
        if (!write_res) return;
        (void)port_obj->port->flush();
    }

    void flush_() override {
        auto* port_obj = resolve_output_port();
        if (!port_obj) return;
        (void)port_obj->port->flush();
    }

private:
    bool heap_is_alive() const { return heap_alive_->load(std::memory_order_acquire); }

    runtime::types::PortObject* resolve_output_port() {
        using namespace runtime::nanbox;
        if (!heap_is_alive()) return nullptr;
        LispVal port_val = fixed_port_;
        if (current_error_sink_) {
            if (!vm_ || !vm_alive_ || !vm_alive_->load(std::memory_order_acquire)) return nullptr;
            port_val = vm_->current_error_port();
        }

        if (!ops::is_boxed(port_val) || ops::tag(port_val) != Tag::HeapObject) return nullptr;
        auto* port_obj = heap_.try_get_as<runtime::memory::heap::ObjectKind::Port, runtime::types::PortObject>(
            ops::payload(port_val));
        if (!port_obj) return nullptr;
        if (!port_obj->port || !port_obj->port->is_output()) return nullptr;
        return port_obj;
    }

    runtime::memory::heap::Heap& heap_;
    std::shared_ptr<const std::atomic<bool>> heap_alive_;
    runtime::vm::VM* vm_{nullptr};
    std::shared_ptr<const std::atomic<bool>> vm_alive_;
    runtime::nanbox::LispVal fixed_port_{runtime::nanbox::Nil};
    bool current_error_sink_{false};
};

} ///< namespace eta::log
