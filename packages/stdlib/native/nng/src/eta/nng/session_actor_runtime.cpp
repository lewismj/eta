/**
 * @file session_actor_runtime.cpp
 * @brief Actor runtime implementation backed by NNG process-manager primitives.
 */

#include "eta/nng/session_actor_runtime.h"

#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

#include "eta/interpreter/module_path.h"
#include "eta/nng/nng_factory.h"
#include "eta/nng/nng_socket_ptr.h"
#include "eta/runtime/factory.h"
#include "eta/nng/process_mgr.h"
#include "eta/nng/spawn_capture_format.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/session/driver.h"

namespace eta::nng {

namespace fs = std::filesystem;
using eta::interpreter::ModulePathResolver;

namespace {

class NngProcessManagerAdapter final : public native::ActorProcessManager {
public:
    using ThreadDebugListener = native::ActorProcessManager::ThreadDebugListener;

    void set_debug_listener(ThreadDebugListener listener) override {
        if (!listener) {
            proc_mgr_.set_debug_listener({});
            return;
        }

        proc_mgr_.set_debug_listener(
            [listener = std::move(listener)](const ProcessManager::ThreadDebugEvent& ev) {
                native::ActorThreadDebugEvent out;
                out.kind = (ev.kind == ProcessManager::ThreadDebugEvent::Kind::Started)
                    ? native::ActorThreadDebugEvent::Kind::Started
                    : native::ActorThreadDebugEvent::Kind::Exited;
                out.index = ev.index;
                out.vm = ev.vm;
                out.driver = ev.driver;
                out.name = ev.name;
                try {
                    listener(out);
                } catch (...) {}
            });
    }

    [[nodiscard]] std::vector<native::ActorChildInfo> list_children() const override {
        std::vector<native::ActorChildInfo> out;
        const auto children = proc_mgr_.list_children();
        out.reserve(children.size());
        for (const auto& child : children) {
            native::ActorChildInfo info;
            info.pid = child.pid;
            info.endpoint = child.endpoint;
            info.module_path = child.module_path;
            info.alive = child.alive;
            out.push_back(std::move(info));
        }
        return out;
    }

    [[nodiscard]] std::vector<native::ActorThreadInfo> list_threads() const override {
        std::vector<native::ActorThreadInfo> out;
        const auto threads = proc_mgr_.list_threads();
        out.reserve(threads.size());
        for (const auto& thread : threads) {
            native::ActorThreadInfo info;
            info.index = thread.index;
            info.endpoint = thread.endpoint;
            info.module_path = thread.module_path;
            info.func_name = thread.func_name;
            info.alive = thread.alive;
            out.push_back(std::move(info));
        }
        return out;
    }

    [[nodiscard]] bool terminate_thread_by_index(int index) override {
        return proc_mgr_.terminate_thread_by_index(index);
    }

    [[nodiscard]] bool is_thread_alive(runtime::nanbox::LispVal socket) const override {
        return proc_mgr_.is_thread_alive(socket);
    }

    [[nodiscard]] int join_thread(runtime::nanbox::LispVal socket) override {
        return proc_mgr_.join_thread(socket);
    }

    void notify_thread_started(void* vm, void* driver, std::string name) override {
        proc_mgr_.notify_thread_started(vm, driver, std::move(name));
    }

    void notify_thread_exited(void* vm) override {
        proc_mgr_.notify_thread_exited(vm);
    }

    [[nodiscard]] void* native_handle() noexcept override {
        return &proc_mgr_;
    }

    [[nodiscard]] const void* native_handle() const noexcept override {
        return &proc_mgr_;
    }

    [[nodiscard]] ProcessManager& native_process_manager() noexcept {
        return proc_mgr_;
    }

private:
    ProcessManager proc_mgr_;
};

} // namespace

class SessionActorRuntime::Impl {
public:
    NngProcessManagerAdapter process_manager;
    runtime::nanbox::LispVal mailbox_val{runtime::nanbox::Nil};
};

SessionActorRuntime::SessionActorRuntime()
    : impl_(std::make_unique<Impl>()) {}

SessionActorRuntime::~SessionActorRuntime() = default;

bool SessionActorRuntime::install_mailbox(runtime::memory::heap::Heap& heap,
                                          const std::string& endpoint) {
    NngSocketPtr sp;
    sp.protocol = NngProtocol::Pair;
    int rv = nng_pair0_open(&sp.socket);
    if (rv != 0) return false;

    nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 1000);

    rv = nng_dial(sp.socket, endpoint.c_str(), nullptr, 0);
    if (rv != 0) return false;
    sp.dialed = true;
    sp.endpoint_hint = endpoint;

    auto val = factory::make_nng_socket(heap, std::move(sp));
    if (!val) return false;
    impl_->mailbox_val = *val;
    return true;
}

void SessionActorRuntime::install_worker_factories(const std::string& module_search_path,
                                                   StreamSink stdout_sink,
                                                   StreamSink stderr_sink) {
    auto* process_manager = &impl_->process_manager;
    ProcessManager::ThreadWorkerFn thread_worker_fn =
        [module_search_path, process_manager, stdout_sink, stderr_sink](
            const std::string& th_module_path,
            const std::string& th_func_name,
            const std::string& th_endpoint,
            std::vector<std::string> th_text_args,
            std::shared_ptr<std::atomic<bool>> alive) noexcept
    {
        try {
            auto resolver = ModulePathResolver::from_path_string(module_search_path);
            const auto child_heap = session::Driver::parse_heap_env_var(
                "ETA_HEAP_SOFT_LIMIT_CHILD_THREADS",
                session::Driver::parse_heap_env_var(
                    "ETA_HEAP_SOFT_LIMIT",
                    session::Driver::DEFAULT_CHILD_HEAP_SOFT_LIMIT_BYTES));
            session::Driver child(std::move(resolver), child_heap);

            if (stdout_sink || stderr_sink) {
                child.set_stream_sinks(stdout_sink, stderr_sink);
            }

            const auto child_sidecar_dir = fs::path(th_module_path).parent_path();
            if (!child.load_package_sidecars(child_sidecar_dir)) {
                alive->store(false, std::memory_order_release);
                return;
            }

            if (!child.install_mailbox(th_endpoint)) {
                alive->store(false, std::memory_order_release);
                return;
            }

            /// Load the target module
            if (!child.run_file(fs::path(th_module_path))) {
                alive->store(false, std::memory_order_release);
                return;
            }

            /**
             * Notify any DAP debug listener now that the child VM/Driver
             * exist and have loaded source. This lets the adapter install
             * its per-thread stop callback and breakpoints before the
             * spawn-thread function actually starts running user code.
             */
            std::string th_name = fs::path(th_module_path).stem().string();
            if (!th_func_name.empty()) th_name += " (" + th_func_name + ")";
            process_manager->notify_thread_started(
                static_cast<void*>(&child.vm()),
                static_cast<void*>(&child),
                std::move(th_name));

            /// Build and evaluate: (func-name arg1 arg2 ...)
            std::string call_src = "(" + th_func_name;
            for (const auto& a : th_text_args) {
                call_src += " ";
                call_src += a;
            }
            call_src += ")";
            child.run_source(call_src);
            process_manager->notify_thread_exited(static_cast<void*>(&child.vm()));
        } catch (...) {}
        alive->store(false, std::memory_order_release);
    };

    ProcessManager::ClosureWorkerFn closure_worker_fn =
        [module_search_path, process_manager, stdout_sink, stderr_sink](
            const std::string& th_endpoint,
            ProcessManager::SerializedClosure sc,
            std::shared_ptr<std::atomic<bool>> alive) noexcept
    {
        try {
            auto resolver = ModulePathResolver::from_path_string(module_search_path);
            const auto child_heap = session::Driver::parse_heap_env_var(
                "ETA_HEAP_SOFT_LIMIT_CHILD_THREADS",
                session::Driver::parse_heap_env_var(
                    "ETA_HEAP_SOFT_LIMIT",
                    session::Driver::DEFAULT_CHILD_HEAP_SOFT_LIMIT_BYTES));
            session::Driver child(std::move(resolver), child_heap);

            if (stdout_sink || stderr_sink) {
                child.set_stream_sinks(stdout_sink, stderr_sink);
            }

            std::error_code cwd_ec;
            const auto child_sidecar_dir = fs::current_path(cwd_ec);
            if (!cwd_ec && !child.load_package_sidecars(child_sidecar_dir)) {
                alive->store(false, std::memory_order_release);
                return;
            }

            if (!child.install_mailbox(th_endpoint)) {
                alive->store(false, std::memory_order_release);
                return;
            }

            /**
             * spawn-thread capture payloads may reference primitive globals
             * by fixed primitive slot (SCT_GlobalRef). Ensure the child VM has
             * core and extension primitives installed in slots 0..N-1 before
             * deserializing captures so those references can be resolved.
             */
            auto& child_globals = child.vm().globals();
            const auto child_primitive_slots =
                child.primitive_installer_.total_primitive_count();
            if (child_globals.size() < child_primitive_slots) {
                child_globals.resize(child_primitive_slots,
                                     runtime::nanbox::Nil);
            }
            auto child_install_res = child.primitive_installer_.install_into(
                child_globals, child_primitive_slots);
            if (!child_install_res) {
                alive->store(false, std::memory_order_release);
                return;
            }
            child.primitive_installer_.record_names(
                child.compilation_.mutable_global_names());

            /// Deserialize the function registry from the etac-format blob
            runtime::vm::BytecodeSerializer ser(child.heap(), child.intern_table());
            std::istringstream iss(std::string(sc.funcs_bytes.begin(),
                                               sc.funcs_bytes.end()),
                                   std::ios::binary);
            auto etac_res = ser.deserialize(iss, /*expected_builtins=*/0);
            if (!etac_res) {
                alive->store(false, std::memory_order_release);
                return;
            }
            auto& etac = *etac_res;

            /// Rebase and register the functions in the child's registry
            uint32_t base_idx = static_cast<uint32_t>(child.registry().size());
            for (const auto& func : etac.registry.all()) {
                runtime::vm::BytecodeFunction copy = func;
                copy.rebase_func_indices(static_cast<int32_t>(base_idx));
                child.registry().add(std::move(copy));
            }

            auto capture_payload = deserialize_spawn_capture(
                std::span<const uint8_t>(sc.captures_bytes),
                child.heap(),
                child.intern_table(),
                [&child, base_idx](uint32_t remapped_idx)
                    -> const runtime::vm::BytecodeFunction*
                {
                    return child.registry().get(base_idx + remapped_idx);
                },
                [&child](uint32_t slot) -> std::optional<runtime::nanbox::LispVal>
                {
                    const auto& globals = child.vm().globals();
                    if (slot >= globals.size()) return std::nullopt;
                    return globals[slot];
                });
            if (!capture_payload) {
                alive->store(false, std::memory_order_release);
                return;
            }

            /// Hydrate captured globals before executing the thunk.
            auto& globals = child.vm().globals();
            for (const auto& cg : capture_payload->globals) {
                if (globals.size() <= cg.slot) globals.resize(cg.slot + 1, runtime::nanbox::Nil);
                globals[cg.slot] = cg.value;
            }

            /// Reconstruct the Closure heap object in the child's heap.
            const auto* entry_func = child.registry().get(base_idx);
            if (!entry_func) {
                alive->store(false, std::memory_order_release);
                return;
            }
            auto closure_val = runtime::memory::factory::make_closure(
                child.heap(), entry_func, std::move(capture_payload->upvals));
            if (!closure_val) {
                alive->store(false, std::memory_order_release);
                return;
            }

            /// Call the thunk with 0 arguments.
            process_manager->notify_thread_started(
                static_cast<void*>(&child.vm()),
                static_cast<void*>(&child),
                "(spawn-thread)");
            auto result = child.vm().call_value(*closure_val, {});
            if (!result) {
            }
            process_manager->notify_thread_exited(static_cast<void*>(&child.vm()));
        } catch (const std::exception&) {
        } catch (...) {
        }
        alive->store(false, std::memory_order_release);
    };

    auto& native_process_manager = impl_->process_manager.native_process_manager();
    native_process_manager.set_worker_factory(std::move(thread_worker_fn));
    native_process_manager.set_closure_factory(std::move(closure_worker_fn));
}

void SessionActorRuntime::on_actor_lifecycle(std::function<void(const ActorEvent&)> on_event) {
    if (!on_event) {
        impl_->process_manager.set_debug_listener({});
        return;
    }

    impl_->process_manager.set_debug_listener(
        [cb = std::move(on_event)](const native::ActorThreadDebugEvent& ev) {
            ActorEvent out;
            out.kind = (ev.kind == native::ActorThreadDebugEvent::Kind::Started)
                ? ActorEvent::Kind::Started
                : ActorEvent::Kind::Exited;
            out.index = ev.index;
            out.name = ev.name;
            try {
                cb(out);
            } catch (...) {}
        });
}

native::ActorProcessManager* SessionActorRuntime::process_manager() noexcept {
    return &impl_->process_manager;
}

const native::ActorProcessManager* SessionActorRuntime::process_manager() const noexcept {
    return &impl_->process_manager;
}

runtime::nanbox::LispVal SessionActorRuntime::mailbox() const noexcept {
    return impl_->mailbox_val;
}

runtime::nanbox::LispVal* SessionActorRuntime::mailbox_slot() noexcept {
    return &impl_->mailbox_val;
}

std::unique_ptr<native::ActorRuntime> make_session_actor_runtime() {
    return std::make_unique<SessionActorRuntime>();
}

} // namespace eta::nng
