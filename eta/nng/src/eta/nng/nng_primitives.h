#pragma once

/// @file nng_primitives.h
/// @brief Register nng socket primitives into an Eta BuiltinEnvironment.
///
/// Provides:  nng-socket  nng-listen  nng-dial  nng-close  nng-socket?
///            send!  recv!  nng-poll  nng-subscribe  nng-set-option
///            spawn  spawn-kill  spawn-wait  current-mailbox
///            monitor  demonitor  enable-heartbeat
///
/// Registration order MUST match builtin_names.h (ETA_HAS_NNG section).
/// All primitives capture Heap& and InternTable& by reference.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <queue>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>
#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/pipeline0/push.h>
#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/survey0/survey.h>
#include <nng/protocol/survey0/respond.h>
#include <nng/protocol/bus0/bus.h>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/types/types.h>
#include <eta/runtime/vm/bytecode_serializer.h>
#include <eta/semantics/emitter.h>

#include "nng_socket_ptr.h"
#include "nng_factory.h"
#include "wire_format.h"
#include "process_mgr.h"

namespace eta::nng {

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::factory;
using Args = const std::vector<LispVal>&;

// Helpers

/// Build a nng-error runtime error from an nng return code.
inline std::unexpected<RuntimeError> nng_error(int rv) {
    return std::unexpected(
        RuntimeError{VMError{RuntimeErrorCode::InternalError,
            "nng-error: " + std::string(nng_strerror(rv))}});
}

/// Get an NngSocketPtr from a LispVal. Returns nullptr if wrong type.
inline NngSocketPtr* get_socket(Heap& heap, LispVal v) {
    if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
    return heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(v));
}

/// Get the string value of a symbol LispVal via the intern table.
inline std::optional<std::string> symbol_to_string(LispVal v, InternTable& intern) {
    if (!ops::is_boxed(v) || ops::tag(v) != Tag::Symbol) return std::nullopt;
    auto sv = intern.get_string(ops::payload(v));
    if (!sv) return std::nullopt;
    return std::string(*sv);
}

/// Get the protocol name string for display.
inline const char* protocol_name(NngProtocol p) noexcept {
    switch (p) {
        case NngProtocol::Pair:       return "pair";
        case NngProtocol::Req:        return "req";
        case NngProtocol::Rep:        return "rep";
        case NngProtocol::Pub:        return "pub";
        case NngProtocol::Sub:        return "sub";
        case NngProtocol::Push:       return "push";
        case NngProtocol::Pull:       return "pull";
        case NngProtocol::Surveyor:   return "surveyor";
        case NngProtocol::Respondent: return "respondent";
        case NngProtocol::Bus:        return "bus";
        default:                      return "unknown";
    }
}

/// Open a new nng socket for the given protocol. Returns 0 on success.
inline int open_socket(nng_socket& sock, NngProtocol proto) {
    switch (proto) {
        case NngProtocol::Pair:       return nng_pair0_open(&sock);
        case NngProtocol::Req:        return nng_req0_open(&sock);
        case NngProtocol::Rep:        return nng_rep0_open(&sock);
        case NngProtocol::Pub:        return nng_pub0_open(&sock);
        case NngProtocol::Sub:        return nng_sub0_open(&sock);
        case NngProtocol::Push:       return nng_push0_open(&sock);
        case NngProtocol::Pull:       return nng_pull0_open(&sock);
        case NngProtocol::Surveyor:   return nng_surveyor0_open(&sock);
        case NngProtocol::Respondent: return nng_respondent0_open(&sock);
        case NngProtocol::Bus:        return nng_bus0_open(&sock);
        default:                      return NNG_ENOTSUP;
    }
}

/// Parse a symbol to an NngProtocol. Returns std::nullopt on unknown.
inline std::optional<NngProtocol> parse_protocol(const std::string& name) {
    if (name == "pair")       return NngProtocol::Pair;
    if (name == "req")        return NngProtocol::Req;
    if (name == "rep")        return NngProtocol::Rep;
    if (name == "pub")        return NngProtocol::Pub;
    if (name == "sub")        return NngProtocol::Sub;
    if (name == "push")       return NngProtocol::Push;
    if (name == "pull")       return NngProtocol::Pull;
    if (name == "surveyor")   return NngProtocol::Surveyor;
    if (name == "respondent") return NngProtocol::Respondent;
    if (name == "bus")        return NngProtocol::Bus;
    return std::nullopt;
}

// Monitoring helpers

/// Return current time as milliseconds since the steady_clock epoch.
inline int64_t current_epoch_ms() noexcept {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// Static pipe-removal callback registered by `monitor`.
/// arg == raw NngSocketPtr* (stable heap address).
/// Called from nng's internal I/O thread on peer disconnect.
static void monitor_pipe_cb(nng_pipe /*pipe*/, nng_pipe_ev /*ev*/, void* arg) noexcept {
    auto* sp = static_cast<NngSocketPtr*>(arg);
    if (!sp || !sp->monitor_state) return;
    auto& ms = *sp->monitor_state;
    if (ms.closing_normally.load(std::memory_order_acquire)) return;
    if (!ms.monitored) return;
    if (ms.monitor_down_msg.empty()) return;
    ms.push_notification(ms.monitor_down_msg);
}

// register_nng_primitives

// Thread closure serialization helpers

/// Collect all function indices reachable from @p entry_idx via MakeClosure
/// constants (BFS).  The entry function is always first in the returned list.
inline std::vector<uint32_t> collect_closure_deps(
    uint32_t entry_idx,
    const semantics::BytecodeFunctionRegistry& reg)
{
    std::vector<uint32_t> order;
    std::unordered_set<uint32_t> visited;
    std::queue<uint32_t> todo;
    todo.push(entry_idx);
    while (!todo.empty()) {
        auto idx = todo.front(); todo.pop();
        if (!visited.insert(idx).second) continue;
        order.push_back(idx);
        const auto* f = reg.get(idx);
        if (!f) continue;
        for (const auto& c : f->constants) {
            if (runtime::vm::is_func_index(c))
                todo.push(runtime::vm::decode_func_index(c));
        }
    }
    return order;
}

/// Build a SerializedClosure from a closure object's entry function and upvalues.
///
/// Collects all reachable function bytecode (entry first), remaps function-index
/// constants to a 0-based mini-registry, serializes via BytecodeSerializer, and
/// serializes each upvalue via the binary wire format.
///
/// Returns nullopt if any upvalue is non-serializable (closure, port, socket…).
inline std::optional<ProcessManager::SerializedClosure> build_serialized_closure(
    uint32_t entry_idx,
    const std::vector<LispVal>& upvals,
    const semantics::BytecodeFunctionRegistry& src_reg,
    Heap& heap,
    InternTable& intern)
{
    // 1. Collect reachable functions (entry at index 0)
    auto order = collect_closure_deps(entry_idx, src_reg);

    // 2. Build index remap: old_global → new_0based
    std::unordered_map<uint32_t, uint32_t> remap;
    for (uint32_t i = 0; i < static_cast<uint32_t>(order.size()); ++i)
        remap[order[i]] = i;

    // 3. Build new registry with rebased function-index constants
    semantics::BytecodeFunctionRegistry new_reg;
    for (uint32_t old_idx : order) {
        const auto* f = src_reg.get(old_idx);
        if (!f) return std::nullopt;
        runtime::vm::BytecodeFunction copy = *f;
        for (auto& c : copy.constants) {
            if (runtime::vm::is_func_index(c)) {
                auto it = remap.find(runtime::vm::decode_func_index(c));
                if (it == remap.end()) return std::nullopt;
                c = runtime::vm::encode_func_index(it->second);
            }
        }
        new_reg.add(std::move(copy));
    }

    // 4. Serialize the mini-registry as etac bytes
    runtime::vm::BytecodeSerializer ser(heap, intern);
    runtime::vm::ModuleEntry mod;
    mod.name             = "__spawn_thread__";
    mod.init_func_index  = 0; // entry is always index 0
    mod.total_globals    = 0;

    std::ostringstream oss(std::ios::binary);
    // Pass num_builtins=0 to skip builtin-count validation on deserialization
    if (!ser.serialize({mod}, new_reg, 0, false, oss, {}, 0))
        return std::nullopt;

    ProcessManager::SerializedClosure sc;
    auto str = oss.str();
    sc.funcs_bytes = std::vector<uint8_t>(str.begin(), str.end());

    // 5. Serialize upvalues via binary wire format
    for (const auto& uv : upvals) {
        auto bin = serialize_binary(uv, heap, intern);
        if (bin.empty()) return std::nullopt; // non-serializable upvalue
        sc.upvals.push_back(std::move(bin));
    }
    return sc;
}

/// Register all nng primitives into the given BuiltinEnvironment.
///
/// Process-manager parameters are optional; omitting them leaves spawn/mailbox
/// primitives registered (so arity checking still works in the LSP/analyzer)
/// but they return a clear error when actually called without a process manager.
///
/// @param proc_mgr          ProcessManager owned by the Driver. nullptr = no spawning.
/// @param etai_path         Full path to the etai executable. Empty = no spawning.
/// @param mailbox_val       Pointer to the Driver's mailbox_val_ field (child only).
/// @param module_search_path Colon/semicolon-separated module search path to
///                           propagate to spawned children via ETA_MODULE_PATH
///                           (only if ETA_MODULE_PATH is not already set in env).
/// @param thread_worker_fn  Factory for in-process actor threads.
/// @param func_registry     Pointer to the Driver's function registry, required
///                           for spawn-thread. nullptr disables it.
///
/// Registration order MUST match the ETA_HAS_NNG section in builtin_names.h.
inline void register_nng_primitives(
    BuiltinEnvironment& env, Heap& heap, InternTable& intern,
    ProcessManager* proc_mgr           = nullptr,
    std::string     etai_path          = {},
    LispVal*        mailbox_val        = nullptr,
    std::string     module_search_path = {},
    ProcessManager::ThreadWorkerFn thread_worker_fn = {},
    semantics::BytecodeFunctionRegistry* func_registry = nullptr)
{
    // nng-socket
    // (nng-socket type-symbol) → socket
    env.register_builtin("nng-socket", 1, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto sym = symbol_to_string(args[0], intern);
            if (!sym) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "nng-socket: expected a symbol, e.g. 'pair"}});
            }
            auto proto = parse_protocol(*sym);
            if (!proto) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "nng-socket: unknown protocol '" + *sym +
                    "'; expected pair|req|rep|pub|sub|push|pull|surveyor|respondent|bus"}});
            }

            NngSocketPtr sp;
            sp.protocol = *proto;
            int rv = open_socket(sp.socket, *proto);
            if (rv != 0) return nng_error(rv);

            // Set default recv timeout of 1000 ms to avoid blocking the VM forever.
            nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 1000);

            return factory::make_nng_socket(heap, std::move(sp));
        });

    // nng-listen
    // (nng-listen sock endpoint) → sock
    env.register_builtin("nng-listen", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-listen: expected an nng-socket"}});
            }
            if (sp->closed) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "nng-listen: socket is already closed"}});
            }
            if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::String) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-listen: expected a string endpoint"}});
            }
            auto ep_sv = intern.get_string(ops::payload(args[1]));
            if (!ep_sv) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "nng-listen: failed to resolve endpoint string"}});
            }
            std::string endpoint(*ep_sv);

            int rv = nng_listen(sp->socket, endpoint.c_str(), nullptr, 0);
            if (rv != 0) return nng_error(rv);
            sp->listening = true;
            sp->endpoint_hint = endpoint;  // store endpoint for supervision down-messages
            return args[0]; // return the socket itself
        });

    // nng-dial
    // (nng-dial sock endpoint) → sock
    env.register_builtin("nng-dial", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-dial: expected an nng-socket"}});
            }
            if (sp->closed) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "nng-dial: socket is already closed"}});
            }
            if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::String) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-dial: expected a string endpoint"}});
            }
            auto ep_sv = intern.get_string(ops::payload(args[1]));
            if (!ep_sv) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "nng-dial: failed to resolve endpoint string"}});
            }
            std::string endpoint(*ep_sv);

            int rv = nng_dial(sp->socket, endpoint.c_str(), nullptr, 0);
            if (rv != 0) return nng_error(rv);
            sp->dialed = true;
            if (sp->endpoint_hint.empty()) sp->endpoint_hint = endpoint;  // store endpoint for supervision down-messages
            return args[0]; // return the socket itself
        });

    // nng-close
    // (nng-close sock) → #t (idempotent)
    env.register_builtin("nng-close", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-close: expected an nng-socket"}});
            }
            if (!sp->closed) {
                // Signal closing_normally before nng_close to suppress
                // spurious disconnect notifications via monitor_pipe_cb.
                if (sp->monitor_state) {
                    sp->monitor_state->closing_normally.store(true, std::memory_order_release);
                    if (sp->monitor_state->heartbeat)
                        sp->monitor_state->heartbeat->stop.store(true, std::memory_order_release);
                }
                nng_close(sp->socket);
                sp->socket = NNG_SOCKET_INITIALIZER;
                sp->closed = true;
            }
            return nanbox::True;
        });

    // nng-socket?
    // (nng-socket? x) → #t/#f
    env.register_builtin("nng-socket?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            return sp ? nanbox::True : nanbox::False;
        });

    // send!
    // (send! sock value [flag]) → #t on success, #f if EAGAIN (noblock)
    // flag: 'noblock → NNG_FLAG_NONBLOCK
    //       'wait    → blocking (no timeout change)
    //       'text    → use s-expression text format instead of binary
    // Default serialisation format: binary.
    env.register_builtin("send!", 2, true,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "send!: expected an nng-socket"}});
            }
            if (sp->closed) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "send!: socket is closed"}});
            }

            // Parse optional flag argument
            int flags = 0;
            bool wait_mode = false;
            bool use_text  = false;
            if (args.size() >= 3) {
                auto flag_sym = symbol_to_string(args[2], intern);
                if (flag_sym) {
                    if      (*flag_sym == "noblock") flags = NNG_FLAG_NONBLOCK;
                    else if (*flag_sym == "wait")    wait_mode = true;
                    else if (*flag_sym == "text")    use_text  = true;
                }
            }

            // Serialize: binary by default, text when 'text flag given
            std::vector<uint8_t> bin_buf;
            std::string          text_buf;
            const void*  msg_ptr;
            size_t       msg_size;
            if (use_text) {
                text_buf = serialize_value(args[1], heap, intern);
                msg_ptr  = text_buf.data();
                msg_size = text_buf.size();
            } else {
                bin_buf  = serialize_binary(args[1], heap, intern);
                msg_ptr  = bin_buf.data();
                msg_size = bin_buf.size();
            }

            // Optionally override timeout for 'wait
            nng_duration saved_timeout = 0;
            if (wait_mode) {
                nng_socket_get_ms(sp->socket, NNG_OPT_SENDTIMEO, &saved_timeout);
                nng_socket_set_ms(sp->socket, NNG_OPT_SENDTIMEO, NNG_DURATION_INFINITE);
            }

            int rv = nng_send(sp->socket,
                              const_cast<void*>(msg_ptr),
                              msg_size,
                              flags);

            if (wait_mode) {
                nng_socket_set_ms(sp->socket, NNG_OPT_SENDTIMEO, saved_timeout);
            }

            if (rv == NNG_EAGAIN) return nanbox::False;
            if (rv != 0) return nng_error(rv);
            return nanbox::True;
        });

    // recv!
    // (recv! sock [flag]) → LispVal or #f on timeout/EAGAIN
    // flag: 'noblock → NNG_FLAG_NONBLOCK; 'wait → blocking
    //
    // Checks monitor/heartbeat notifications first.
    // Heartbeat ping/pong control messages (0xEB prefix) are handled
    // transparently: pings receive an auto-reply pong, pongs update the
    // heartbeat timestamp.  Up to 16 consecutive heartbeat messages are
    // filtered before returning #f.
    env.register_builtin("recv!", 1, true,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "recv!: expected an nng-socket"}});
            }
            if (sp->closed) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "recv!: socket is closed"}});
            }

            // Check monitor/heartbeat notifications first (thread-safe).
            if (sp->monitor_state) {
                auto notif = sp->monitor_state->pop_notification();
                if (notif) {
                    auto& data = *notif;
                    std::expected<LispVal, RuntimeError> res;
                    if (is_binary_format(data.data(), data.size())) {
                        res = deserialize_binary(std::span<const uint8_t>(data), heap, intern);
                    } else {
                        std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
                        res = deserialize_value(sv, heap, intern);
                    }
                    if (!res) return std::unexpected(res.error());
                    return *res;
                }
            }

            // Check the pending message queue (messages saved by nng-poll).
            if (!sp->pending_msgs.empty()) {
                auto data = std::move(sp->pending_msgs.front());
                sp->pending_msgs.pop_front();
                // Heartbeat check on buffered messages
                if (is_heartbeat_ping(data.data(), data.size())) {
                    auto pong = make_heartbeat_pong();
                    nng_send(sp->socket, pong.data(), pong.size(), NNG_FLAG_NONBLOCK);
                    // Return #f — caller can retry; this avoids infinite recursion
                    return nanbox::False;
                }
                if (is_heartbeat_pong(data.data(), data.size())) {
                    if (sp->monitor_state && sp->monitor_state->heartbeat)
                        sp->monitor_state->heartbeat->last_pong_epoch_ms.store(current_epoch_ms());
                    return nanbox::False;
                }
                std::expected<LispVal, RuntimeError> result;
                if (is_binary_format(data.data(), data.size())) {
                    result = deserialize_binary(std::span<const uint8_t>(data), heap, intern);
                } else {
                    std::string_view sv(reinterpret_cast<const char*>(data.data()), data.size());
                    result = deserialize_value(sv, heap, intern);
                }
                if (!result) return std::unexpected(result.error());
                return *result;
            }

            // Parse optional flag argument
            int flags = 0;
            bool wait_mode = false;
            if (args.size() >= 2) {
                auto flag_sym = symbol_to_string(args[1], intern);
                if (flag_sym) {
                    if      (*flag_sym == "noblock") flags = NNG_FLAG_NONBLOCK;
                    else if (*flag_sym == "wait")    wait_mode = true;
                }
            }

            // Override timeout for 'wait mode
            nng_duration saved_timeout = 0;
            if (wait_mode) {
                nng_socket_get_ms(sp->socket, NNG_OPT_RECVTIMEO, &saved_timeout);
                nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, NNG_DURATION_INFINITE);
            }

            // Inner recv loop: filter up to 16 heartbeat messages transparently.
            constexpr int MAX_HB_SKIP = 16;
            int hb_skipped = 0;
        recv_loop:
            void* buf = nullptr;
            size_t sz = 0;
            int rv = nng_recv(sp->socket, &buf, &sz, flags | NNG_FLAG_ALLOC);

            if (wait_mode && rv != 0) {
                nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, saved_timeout);
            }

            if (rv == NNG_EAGAIN || rv == NNG_ETIMEDOUT) return nanbox::False;
            if (rv != 0) {
                if (wait_mode) nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, saved_timeout);
                return nng_error(rv);
            }

            // Transparent heartbeat handling
            if (is_heartbeat_ping(static_cast<const uint8_t*>(buf), sz)) {
                nng_free(buf, sz);
                auto pong = make_heartbeat_pong();
                nng_send(sp->socket, pong.data(), pong.size(), NNG_FLAG_NONBLOCK);
                if (++hb_skipped < MAX_HB_SKIP) goto recv_loop;
                if (wait_mode) nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, saved_timeout);
                return nanbox::False;
            }
            if (is_heartbeat_pong(static_cast<const uint8_t*>(buf), sz)) {
                nng_free(buf, sz);
                if (sp->monitor_state && sp->monitor_state->heartbeat)
                    sp->monitor_state->heartbeat->last_pong_epoch_ms.store(current_epoch_ms());
                if (++hb_skipped < MAX_HB_SKIP) goto recv_loop;
                if (wait_mode) nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, saved_timeout);
                return nanbox::False;
            }

            if (wait_mode) {
                nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, saved_timeout);
            }


            // Auto-detect binary vs text format
            std::expected<LispVal, RuntimeError> result;
            if (is_binary_format(static_cast<const uint8_t*>(buf), sz)) {
                std::span<const uint8_t> sp_data(static_cast<const uint8_t*>(buf), sz);
                result = deserialize_binary(sp_data, heap, intern);
            } else {
                std::string_view sv(static_cast<const char*>(buf), sz);
                result = deserialize_value(sv, heap, intern);
            }
            nng_free(buf, sz);
            if (!result) return std::unexpected(result.error());
            return *result;
        });

    // nng-poll
    // (nng-poll items timeout-ms) → list of ready sockets
    // items: list of (socket . events) pairs  (events ignored for now)
    // Checks each socket with non-blocking recv; buffers any received
    // message into the socket's pending_msgs queue and marks it as ready.
    env.register_builtin("nng-poll", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            // Parse timeout-ms
            int64_t timeout_ms = 0;
            {
                auto n = classify_numeric(args[1], heap);
                if (n.is_fixnum()) timeout_ms = n.int_val;
                else if (n.is_flonum()) timeout_ms = static_cast<int64_t>(n.float_val);
            }

            // Collect sockets from the items list
            std::vector<LispVal> sockets;
            LispVal lst = args[0];
            while (lst != nanbox::Nil && ops::is_boxed(lst) && ops::tag(lst) == Tag::HeapObject) {
                auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(lst));
                if (!cons) break;
                // Each element is (socket . events)
                LispVal item = cons->car;
                LispVal sock_val;
                if (ops::is_boxed(item) && ops::tag(item) == Tag::HeapObject) {
                    auto* pair = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(item));
                    if (pair) {
                        sock_val = pair->car; // (socket . events) → socket
                    } else {
                        sock_val = item; // bare socket
                    }
                } else {
                    sock_val = item;
                }
                sockets.push_back(sock_val);
                lst = cons->cdr;
            }

            // Deadline for the poll
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(timeout_ms);

            // Check each socket non-blocking; retry until timeout_ms elapses
            std::vector<LispVal> ready;
            bool checked_once = false;

            while (true) {
                for (auto& sv : sockets) {
                    auto* sp = get_socket(heap, sv);
                    if (!sp || sp->closed) continue;

                    // Already has pending data → ready immediately
                    if (!sp->pending_msgs.empty()) {
                        // Only add once
                        bool already = false;
                        for (auto& r : ready) if (r == sv) { already = true; break; }
                        if (!already) ready.push_back(sv);
                        continue;
                    }

                    // Try non-blocking recv
                    void* buf = nullptr;
                    size_t sz = 0;
                    int rv = nng_recv(sp->socket, &buf, &sz, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC);
                    if (rv == 0) {
                        // Got a message — buffer it
                        std::vector<uint8_t> data(static_cast<uint8_t*>(buf),
                                                   static_cast<uint8_t*>(buf) + sz);
                        nng_free(buf, sz);
                        sp->pending_msgs.push_back(std::move(data));

                        bool already = false;
                        for (auto& r : ready) if (r == sv) { already = true; break; }
                        if (!already) ready.push_back(sv);
                    }
                    // NNG_EAGAIN → not ready, skip
                }

                checked_once = true;
                if (!ready.empty() || timeout_ms == 0) break;
                if (std::chrono::steady_clock::now() >= deadline) break;

                // Small sleep to avoid spinning
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            (void)checked_once;

            // Build the result list from back to front
            LispVal result = nanbox::Nil;
            for (int i = static_cast<int>(ready.size()) - 1; i >= 0; --i) {
                auto cons_res = make_cons(heap, ready[static_cast<size_t>(i)], result);
                if (!cons_res) return std::unexpected(cons_res.error());
                result = *cons_res;
            }
            return result;
        });

    // nng-subscribe
    // (nng-subscribe sock topic) → #t
    // topic is a string prefix filter for SUB sockets
    env.register_builtin("nng-subscribe", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-subscribe: expected an nng-socket"}});
            }
            if (sp->closed) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "nng-subscribe: socket is closed"}});
            }
            if (sp->protocol != NngProtocol::Sub) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "nng-subscribe: socket must be a 'sub socket"}});
            }

            // Get the topic string
            std::string topic;
            if (ops::is_boxed(args[1]) && ops::tag(args[1]) == Tag::String) {
                auto sv = intern.get_string(ops::payload(args[1]));
                if (sv) topic = std::string(*sv);
            } else if (ops::is_boxed(args[1]) && ops::tag(args[1]) == Tag::Symbol) {
                auto sv = intern.get_string(ops::payload(args[1]));
                if (sv) topic = std::string(*sv);
            }

            int rv = nng_socket_set(sp->socket, NNG_OPT_SUB_SUBSCRIBE,
                                    topic.c_str(), topic.size());
            if (rv != 0) return nng_error(rv);
            return nanbox::True;
        });

    // nng-set-option
    // (nng-set-option sock option value) → #t
    // option symbols: 'recv-timeout 'send-timeout 'recv-buf-size 'survey-time
    env.register_builtin("nng-set-option", 3, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-set-option: expected an nng-socket"}});
            }
            if (sp->closed) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "nng-set-option: socket is closed"}});
            }

            auto opt_sym = symbol_to_string(args[1], intern);
            if (!opt_sym) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-set-option: option must be a symbol"}});
            }

            auto n = classify_numeric(args[2], heap);
            if (!n.is_fixnum() && !n.is_flonum()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-set-option: value must be a number"}});
            }
            int64_t ival = n.is_fixnum() ? n.int_val : static_cast<int64_t>(n.float_val);

            int rv = 0;
            if (*opt_sym == "recv-timeout") {
                rv = nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO,
                                       static_cast<nng_duration>(ival));
            } else if (*opt_sym == "send-timeout") {
                rv = nng_socket_set_ms(sp->socket, NNG_OPT_SENDTIMEO,
                                       static_cast<nng_duration>(ival));
            } else if (*opt_sym == "recv-buf-size") {
                rv = nng_socket_set_int(sp->socket, NNG_OPT_RECVBUF,
                                        static_cast<int>(ival));
            } else if (*opt_sym == "survey-time") {
                rv = nng_socket_set_ms(sp->socket, NNG_OPT_SURVEYOR_SURVEYTIME,
                                       static_cast<nng_duration>(ival));
            } else {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "nng-set-option: unknown option '" + *opt_sym +
                    "'; expected recv-timeout|send-timeout|recv-buf-size|survey-time"}});
            }

            if (rv != 0) return nng_error(rv);
            return nanbox::True;
        });

    // spawn
    env.register_builtin("spawn", 1, true,
        [&heap, &intern, proc_mgr, etai_path, module_search_path](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!proc_mgr || etai_path.empty()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn: process manager not configured "
                    "(this build does not support spawning, or etai_path is empty)"}});
            }
            if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::String) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "spawn: expected a string module path"}});
            }
            auto path_sv = intern.get_string(ops::payload(args[0]));
            if (!path_sv) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn: failed to resolve module path string"}});
            }
            return proc_mgr->spawn(std::string(*path_sv), heap, intern,
                                   etai_path, module_search_path);
        });

    // spawn-kill
    // (spawn-kill sock) → #t if signal sent, #f if not found
    env.register_builtin("spawn-kill", 1, false,
        [&heap, proc_mgr](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!proc_mgr) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn-kill: process manager not configured"}});
            }
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "spawn-kill: expected an nng-socket"}});
            }
            return proc_mgr->kill_child(args[0]) ? nanbox::True : nanbox::False;
        });

    // spawn-wait
    // (spawn-wait sock) → exit-code (fixnum) or #f if not found
    env.register_builtin("spawn-wait", 1, false,
        [&heap, proc_mgr](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!proc_mgr) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn-wait: process manager not configured"}});
            }
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "spawn-wait: expected an nng-socket"}});
            }
            int code = proc_mgr->wait_for(args[0]);
            if (code < 0) return nanbox::False;
            return make_fixnum(heap, static_cast<int64_t>(code));
        });

    // current-mailbox
    // (current-mailbox) → the PAIR socket to the parent, or () if not a child
    env.register_builtin("current-mailbox", 0, false,
        [mailbox_val](Args) -> std::expected<LispVal, RuntimeError> {
            if (!mailbox_val) return nanbox::Nil;
            return *mailbox_val;
        });

    // spawn-thread-with
    // (spawn-thread-with module-path func-name args...) → socket
    // Creates a new in-process actor thread with an independent VM.
    // Install the worker factory in proc_mgr now (if provided at registration time);
    // tests may also call proc_mgr->set_worker_factory() before calling this primitive.
    if (proc_mgr && thread_worker_fn) {
        proc_mgr->set_worker_factory(std::move(thread_worker_fn));
    }

    env.register_builtin("spawn-thread-with", 2, true,
        [&heap, &intern, proc_mgr](Args args)
            -> std::expected<LispVal, RuntimeError>
        {
            if (!proc_mgr) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn-thread-with: process manager not configured"}});
            }

            // arg0: module path string
            if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::String) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "spawn-thread-with: first argument must be a module-path string"}});
            }
            auto path_sv = intern.get_string(ops::payload(args[0]));
            if (!path_sv) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn-thread-with: failed to resolve module-path string"}});
            }

            // arg1: function name symbol
            auto func_sym = symbol_to_string(args[1], intern);
            if (!func_sym) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "spawn-thread-with: second argument must be a symbol (function name)"}});
            }

            // remaining args: serialize each as s-expression text
            std::vector<std::string> text_args;
            text_args.reserve(args.size() - 2);
            for (std::size_t i = 2; i < args.size(); ++i) {
                text_args.push_back(serialize_value(args[i], heap, intern));
            }

            return proc_mgr->spawn_thread_with(
                std::string(*path_sv), *func_sym,
                std::move(text_args), heap, intern);
        });

    // spawn-thread
    // (spawn-thread thunk) → socket
    // Serialize the thunk's bytecode + upvalues, then launch a
    // fresh in-process thread that reconstructs and calls the closure.
    env.register_builtin("spawn-thread", 1, false,
        [&heap, &intern, proc_mgr, func_registry](Args args)
            -> std::expected<LispVal, RuntimeError>
        {
            if (!proc_mgr) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn-thread: process manager not configured"}});
            }
            if (!func_registry) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn-thread: function registry not available "
                    "(use spawn-thread-with for file-based workers)"}});
            }

            // Validate: must be a Closure heap object
            if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "spawn-thread: argument must be a 0-argument closure (thunk)"}});
            }
            auto* cl = heap.try_get_as<ObjectKind::Closure,
                                       eta::runtime::types::Closure>(ops::payload(args[0]));
            if (!cl) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "spawn-thread: argument must be a closure (thunk)"}});
            }
            if (cl->func->arity != 0 || cl->func->has_rest) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "spawn-thread: thunk must take 0 arguments (arity=" +
                    std::to_string(cl->func->arity) + ")"}});
            }

            // Reverse-lookup: closure function pointer → registry index
            uint32_t entry_idx = UINT32_MAX;
            {
                auto n = static_cast<uint32_t>(func_registry->size());
                for (uint32_t i = 0; i < n; ++i) {
                    if (func_registry->get(i) == cl->func) { entry_idx = i; break; }
                }
            }
            if (entry_idx == UINT32_MAX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn-thread: closure function not found in registry"}});
            }

            // Serialize the closure (bytecode + upvalues)
            auto sc_opt = build_serialized_closure(
                entry_idx, cl->upvals, *func_registry, heap, intern);
            if (!sc_opt) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "spawn-thread: failed to serialize closure — upvalues must "
                    "be serializable (numbers, strings, symbols, lists, vectors; "
                    "not closures, ports, or sockets)"}});
            }

            return proc_mgr->spawn_thread(std::move(*sc_opt), heap, intern);
        });

    // thread-join
    // (thread-join sock) → 0 on success, #f if not found
    env.register_builtin("thread-join", 1, false,
        [&heap, proc_mgr](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!proc_mgr) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "thread-join: process manager not configured"}});
            }
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "thread-join: expected an nng-socket"}});
            }
            int rc = proc_mgr->join_thread(args[0]);
            if (rc < 0) return nanbox::False;
            return make_fixnum(heap, static_cast<int64_t>(rc));
        });

    // thread-alive?
    // (thread-alive? sock) → #t if the actor thread is still running
    env.register_builtin("thread-alive?", 1, false,
        [&heap, proc_mgr](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!proc_mgr) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "thread-alive?: process manager not configured"}});
            }
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "thread-alive?: expected an nng-socket"}});
            }
            return proc_mgr->is_thread_alive(args[0]) ? nanbox::True : nanbox::False;
        });

    // monitor
    // (monitor sock) → #t
    // Registers a pipe-disconnect callback.  When the remote peer closes the
    // connection, recv! on sock will return '(down endpoint "disconnected").
    env.register_builtin("monitor", 1, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "monitor: expected an nng-socket"}});
            }
            if (sp->closed) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "monitor: socket is already closed"}});
            }

            // Create monitor state on first use
            if (!sp->monitor_state) sp->monitor_state = std::make_shared<MonitorState>();

            // Pre-serialise the down message: (down endpoint "disconnected")
            auto endpoint_str = sp->endpoint_hint.empty() ? "<unknown>" : sp->endpoint_hint;
            auto sym_down   = make_symbol(intern, "down");
            auto str_ep     = make_string(heap, intern, endpoint_str);
            auto str_reason = make_string(heap, intern, "disconnected");
            if (!sym_down || !str_ep || !str_reason) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "monitor: heap allocation failed"}});
            }
            auto tail2 = make_cons(heap, *str_reason, nanbox::Nil);
            auto tail1 = make_cons(heap, *str_ep,     *tail2);
            auto down_list = make_cons(heap, *sym_down, *tail1);
            if (!tail2 || !tail1 || !down_list) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError, "monitor: cons allocation failed"}});
            }
            sp->monitor_state->monitor_down_msg = serialize_binary(*down_list, heap, intern);
            sp->monitor_state->monitored = true;

            // Register the pipe-removal callback.
            // nng_pipe_notify replaces any previous registration for this event.
            nng_pipe_notify(sp->socket, NNG_PIPE_EV_REM_POST,
                            monitor_pipe_cb, static_cast<void*>(sp));
            return nanbox::True;
        });

    // demonitor
    // (demonitor sock) → #t
    // Cancels monitoring on sock.
    env.register_builtin("demonitor", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "demonitor: expected an nng-socket"}});
            }
            if (sp->monitor_state) {
                sp->monitor_state->monitored = false;
                sp->monitor_state->monitor_down_msg.clear();
            }
            if (!sp->closed) {
                // Unregister the callback by passing nullptr
                nng_pipe_notify(sp->socket, NNG_PIPE_EV_REM_POST, nullptr, nullptr);
            }
            return nanbox::True;
        });

    // enable-heartbeat
    // (enable-heartbeat sock interval-ms) → #t
    //
    // Starts a background thread that sends a heartbeat ping every interval-ms
    // milliseconds.  If no pong is received within interval-ms after the ping,
    // a (down endpoint "heartbeat-timeout") message is pushed into the socket's
    // notification queue (returned by the next recv! call).
    //
    // recv! handles pong messages transparently, updating the heartbeat
    // timestamp.  recv! also transparently replies to any incoming pings with
    // a pong (so the remote side can detect liveness too).
    env.register_builtin("enable-heartbeat", 2, false,
        [&heap, &intern](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "enable-heartbeat: expected an nng-socket"}});
            }
            if (sp->closed) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "enable-heartbeat: socket is already closed"}});
            }

            // Parse interval
            int64_t interval_ms = 1000;
            {
                auto n = classify_numeric(args[1], heap);
                if (n.is_fixnum())       interval_ms = n.int_val;
                else if (n.is_flonum())  interval_ms = static_cast<int64_t>(n.float_val);
            }
            if (interval_ms <= 0) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "enable-heartbeat: interval-ms must be positive"}});
            }

            // Create monitor state on first use
            if (!sp->monitor_state) sp->monitor_state = std::make_shared<MonitorState>();

            // Stop any existing heartbeat
            if (sp->monitor_state->heartbeat) {
                sp->monitor_state->heartbeat->stop.store(true, std::memory_order_release);
                if (sp->monitor_state->heartbeat->thread.joinable())
                    sp->monitor_state->heartbeat->thread.detach();
            }

            // Pre-serialise the heartbeat-timeout down message
            auto endpoint_str = sp->endpoint_hint.empty() ? "<unknown>" : sp->endpoint_hint;
            auto sym_down   = make_symbol(intern, "down");
            auto str_ep     = make_string(heap, intern, endpoint_str);
            auto str_reason = make_string(heap, intern, "heartbeat-timeout");
            if (!sym_down || !str_ep || !str_reason) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "enable-heartbeat: heap allocation failed"}});
            }
            auto tail2 = make_cons(heap, *str_reason, nanbox::Nil);
            auto tail1 = make_cons(heap, *str_ep,     *tail2);
            auto down_list = make_cons(heap, *sym_down, *tail1);
            if (!tail2 || !tail1 || !down_list) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "enable-heartbeat: cons allocation failed"}});
            }
            auto down_msg_binary = serialize_binary(*down_list, heap, intern);

            // Build HeartbeatState
            auto hb = std::make_shared<HeartbeatState>();
            hb->interval_ms = interval_ms;
            hb->last_pong_epoch_ms.store(current_epoch_ms());
            sp->monitor_state->heartbeat = hb;

            // Capture everything the thread needs by value.
            // The thread holds shared_ptr ownership of monitor_state and hb so they stay
            // alive until the thread exits.
            nng_socket sock_copy = sp->socket;
            auto monitor_shared = sp->monitor_state;

            hb->thread = std::thread([monitor_shared, sock_copy, interval_ms,
                                       hb, down_msg_binary = std::move(down_msg_binary)]()
            {
                auto sleep_ms = [&](int64_t ms) {
                    constexpr int64_t step = 10;
                    for (int64_t i = 0; i < ms && !hb->stop.load(std::memory_order_acquire); i += step) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(
                            std::min(step, ms - i)));
                    }
                };

                // Wait one interval before the first ping
                sleep_ms(interval_ms);

                while (!hb->stop.load(std::memory_order_acquire)) {
                    // Send ping
                    auto ping = make_heartbeat_ping();
                    nng_send(sock_copy, ping.data(), ping.size(), NNG_FLAG_NONBLOCK);
                    int64_t ping_time = current_epoch_ms();
                    hb->last_ping_epoch_ms.store(ping_time, std::memory_order_release);

                    // Wait one interval for pong
                    sleep_ms(interval_ms);
                    if (hb->stop.load(std::memory_order_acquire)) break;

                    // Check if a pong arrived since the ping was sent
                    int64_t last_pong = hb->last_pong_epoch_ms.load(std::memory_order_acquire);
                    if (last_pong < ping_time) {
                        // No pong within the interval → timeout
                        if (!monitor_shared->closing_normally.load(std::memory_order_acquire)) {
                            monitor_shared->push_notification(down_msg_binary);
                        }
                        break;
                    }
                }
                hb->stop.store(true, std::memory_order_release);
            });

            return nanbox::True;
        });
}

} // namespace eta::nng



