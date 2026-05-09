#pragma once

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
 */
class EtaPortSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    EtaPortSink(runtime::memory::heap::Heap& heap,
                runtime::vm::VM* vm,
                runtime::nanbox::LispVal fixed_port,
                bool current_error_sink)
        : heap_(heap),
          vm_(vm),
          fixed_port_(fixed_port),
          current_error_sink_(current_error_sink) {}

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
    runtime::types::PortObject* resolve_output_port() {
        using namespace runtime::nanbox;
        LispVal port_val = fixed_port_;
        if (current_error_sink_) {
            if (!vm_) return nullptr;
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
    runtime::vm::VM* vm_{nullptr};
    runtime::nanbox::LispVal fixed_port_{runtime::nanbox::Nil};
    bool current_error_sink_{false};
};

} ///< namespace eta::log
