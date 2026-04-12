#pragma once

/// @file nng_primitives.h
/// @brief Register nng socket primitives into an Eta BuiltinEnvironment.
///
/// Provides:  nng-socket  nng-listen  nng-dial  nng-close  nng-socket?
///            send!  recv!  nng-poll  nng-subscribe  nng-set-option
///            spawn  spawn-kill  spawn-wait  current-mailbox      (Phase 4)
///
/// Registration order MUST match builtin_names.h (ETA_HAS_NNG section).
/// All primitives capture Heap& and InternTable& by reference.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <thread>
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

// ─── Helpers ──────────────────────────────────────────────────────────────────

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

// ─── register_nng_primitives ──────────────────────────────────────────────────

/// Register all nng primitives (Phases 3 + 4 + 7) into the given BuiltinEnvironment.
///
/// Phase 4 parameters are optional; omitting them leaves spawn/mailbox
/// primitives registered (so arity checking still works in the LSP/analyzer)
/// but they return a clear error when actually called without a process manager.
///
/// @param proc_mgr          ProcessManager owned by the Driver. nullptr = no spawning.
/// @param etai_path         Full path to the etai executable. Empty = no spawning.
/// @param mailbox_val       Pointer to the Driver's mailbox_val_ field (child only).
/// @param module_search_path Colon/semicolon-separated module search path to
///                           propagate to spawned children via ETA_MODULE_PATH
///                           (only if ETA_MODULE_PATH is not already set in env).
/// @param thread_worker_fn  Factory for in-process actor threads (Phase 7).
///
/// Registration order MUST match the ETA_HAS_NNG section in builtin_names.h.
inline void register_nng_primitives(
    BuiltinEnvironment& env, Heap& heap, InternTable& intern,
    ProcessManager* proc_mgr          = nullptr,
    std::string     etai_path         = {},
    LispVal*        mailbox_val       = nullptr,
    std::string     module_search_path = {},
    ProcessManager::ThreadWorkerFn thread_worker_fn = {})
{
    // ── nng-socket ─────────────────────────────────────────────────────────
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

    // ── nng-listen ─────────────────────────────────────────────────────────
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
            return args[0]; // return the socket itself
        });

    // ── nng-dial ───────────────────────────────────────────────────────────
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
            return args[0]; // return the socket itself
        });

    // ── nng-close ──────────────────────────────────────────────────────────
    // (nng-close sock) → #t (idempotent)
    env.register_builtin("nng-close", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            if (!sp) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "nng-close: expected an nng-socket"}});
            }
            if (!sp->closed) {
                nng_close(sp->socket);
                sp->socket = NNG_SOCKET_INITIALIZER;
                sp->closed = true;
            }
            return nanbox::True;
        });

    // ── nng-socket? ────────────────────────────────────────────────────────
    // (nng-socket? x) → #t/#f
    env.register_builtin("nng-socket?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto* sp = get_socket(heap, args[0]);
            return sp ? nanbox::True : nanbox::False;
        });

    // ── send! ──────────────────────────────────────────────────────────────
    // (send! sock value [flag]) → #t on success, #f if EAGAIN (noblock)
    // flag: 'noblock → NNG_FLAG_NONBLOCK
    //       'wait    → blocking (no timeout change)
    //       'text    → use s-expression text format instead of binary
    // Default serialisation format: binary (Phase 6).
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

    // ── recv! ──────────────────────────────────────────────────────────────
    // (recv! sock [flag]) → LispVal or #f on timeout/EAGAIN
    // flag: 'noblock → NNG_FLAG_NONBLOCK; 'wait → blocking
    //
    // Auto-detects serialisation format: binary messages start with 0xEA
    // (BINARY_VERSION_BYTE); everything else is treated as s-expression text.
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

            // Check the pending message queue first (messages saved by nng-poll).
            // Auto-detect format in buffered messages too.
            if (!sp->pending_msgs.empty()) {
                auto data = std::move(sp->pending_msgs.front());
                sp->pending_msgs.pop_front();
                std::expected<LispVal, RuntimeError> result;
                if (is_binary_format(data.data(), data.size())) {
                    result = deserialize_binary(
                        std::span<const uint8_t>(data), heap, intern);
                } else {
                    std::string_view sv(reinterpret_cast<const char*>(data.data()),
                                        data.size());
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

            void* buf = nullptr;
            size_t sz = 0;
            int rv = nng_recv(sp->socket, &buf, &sz, flags | NNG_FLAG_ALLOC);

            if (wait_mode) {
                nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, saved_timeout);
            }

            if (rv == NNG_EAGAIN || rv == NNG_ETIMEDOUT) return nanbox::False;
            if (rv != 0) return nng_error(rv);

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

    // ── nng-poll ───────────────────────────────────────────────────────────
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

    // ── nng-subscribe ──────────────────────────────────────────────────────
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

    // ── nng-set-option ─────────────────────────────────────────────────────
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

    // ── Phase 4: spawn ─────────────────────────────────────────────────────
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

    // ── spawn-kill ─────────────────────────────────────────────────────────
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

    // ── spawn-wait ─────────────────────────────────────────────────────────
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

    // ── current-mailbox ────────────────────────────────────────────────────
    // (current-mailbox) → the PAIR socket to the parent, or () if not a child
    env.register_builtin("current-mailbox", 0, false,
        [mailbox_val](Args) -> std::expected<LispVal, RuntimeError> {
            if (!mailbox_val) return nanbox::Nil;
            return *mailbox_val;
        });

    // ── Phase 7: spawn-thread-with ─────────────────────────────────────────
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

    // ── spawn-thread ──────────────────────────────────────────────────────
    // (spawn-thread thunk) → Phase 7b — not yet implemented
    // Anonymous closures cannot be serialized; use spawn-thread-with instead.
    env.register_builtin("spawn-thread", 1, false,
        [](Args) -> std::expected<LispVal, RuntimeError> {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InternalError,
                "spawn-thread: anonymous closure serialization is not yet supported "
                "(Phase 7b) — use spawn-thread-with with a named module function instead"}});
        });

    // ── thread-join ───────────────────────────────────────────────────────
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

    // ── thread-alive? ─────────────────────────────────────────────────────
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
}

} // namespace eta::nng

