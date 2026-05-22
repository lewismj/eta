#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eta/runtime/actor/actor_system.h"
#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/nng/wire_format.h"

namespace eta::runtime {

namespace {

struct DecodedExitSignal {
    actor::ActorSystem::ExitReason reason{};
    bool untrappable{false};
};

std::expected<types::Pid, RuntimeError> expect_pid(
    Heap& heap,
    LispVal value,
    const std::string& op_name) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            op_name + ": expected pid"}});
    }

    auto* pid = heap.try_get_as<ObjectKind::Pid, types::Pid>(ops::payload(value));
    if (!pid) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            op_name + ": expected pid"}});
    }

    return *pid;
}

std::optional<types::Pid> try_decode_pid(
    Heap& heap,
    LispVal value) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return std::nullopt;
    }

    auto* pid = heap.try_get_as<ObjectKind::Pid, types::Pid>(ops::payload(value));
    if (!pid) return std::nullopt;
    return *pid;
}

std::expected<types::Pid, RuntimeError> expect_current_pid(
    vm::VM* vm,
    std::string_view op_name) {
    if (!vm || !vm->actor_system()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            std::string(op_name) + ": actor system is not available"}});
    }

    auto current = vm->actor_system()->current_pid();
    if (!current.has_value()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            std::string(op_name) + ": current thread has no actor identity"}});
    }

    return *current;
}

[[nodiscard]] bool symbol_eq(
    LispVal value,
    InternTable& intern_table,
    std::string_view expected) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::Symbol) return false;
    auto name = intern_table.get_string(ops::payload(value));
    return name.has_value() && *name == expected;
}

std::expected<std::string, RuntimeError> decode_registry_name(
    InternTable& intern_table,
    LispVal value,
    std::string_view op_name) {
    if (!ops::is_boxed(value)) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": expected symbol or string name"}});
    }

    const auto tag = ops::tag(value);
    if (tag != Tag::Symbol && tag != Tag::String) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": expected symbol or string name"}});
    }

    auto name = intern_table.get_string(ops::payload(value));
    if (!name.has_value()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": invalid symbol/string name"}});
    }
    return std::string(*name);
}

std::expected<std::string, RuntimeError> decode_text_value(
    InternTable& intern_table,
    LispVal value,
    std::string_view op_name,
    std::string_view value_name) {
    if (!ops::is_boxed(value)) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": expected symbol or string for "
                + std::string(value_name)}});
    }

    const auto tag = ops::tag(value);
    if (tag != Tag::Symbol && tag != Tag::String) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": expected symbol or string for "
                + std::string(value_name)}});
    }

    auto text = intern_table.get_string(ops::payload(value));
    if (!text.has_value()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": invalid " + std::string(value_name)}});
    }
    return std::string(*text);
}

struct NodeOptions {
    std::optional<std::string> node_name{};
    std::optional<std::string> cookie{};
};

std::expected<NodeOptions, RuntimeError> decode_node_options(
    InternTable& intern_table,
    std::span<const LispVal> args,
    std::size_t start_index,
    std::string_view op_name) {
    NodeOptions options;
    if (args.size() <= start_index) return options;

    if (((args.size() - start_index) % 2u) != 0u) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": expected key/value option pairs"}});
    }

    for (std::size_t index = start_index; index < args.size(); index += 2u) {
        auto key = decode_registry_name(intern_table, args[index], op_name);
        if (!key.has_value()) return std::unexpected(key.error());
        auto value = decode_text_value(
            intern_table,
            args[index + 1u],
            op_name,
            "option value");
        if (!value.has_value()) return std::unexpected(value.error());

        if (*key == "name" || *key == "node-name") {
            options.node_name = std::move(*value);
            continue;
        }
        if (*key == "cookie") {
            options.cookie = std::move(*value);
            continue;
        }

        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": unsupported option '" + *key + "'"}});
    }

    if (options.node_name.has_value() != options.cookie.has_value()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name)
                + ": provide both 'node-name (or 'name) and 'cookie when overriding node identity"}});
    }

    return options;
}

std::expected<std::optional<std::chrono::milliseconds>, RuntimeError> decode_receive_timeout(
    Heap& heap,
    InternTable& intern_table,
    LispVal timeout_value) {
    if (timeout_value == nanbox::False) {
        return std::chrono::milliseconds(0);
    }

    if (symbol_eq(timeout_value, intern_table, "wait")) {
        return std::nullopt;
    }

    auto numeric = classify_numeric(timeout_value, heap);
    if (!numeric.is_valid() || !numeric.is_fixnum() || numeric.int_val < 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            "%actor-receive: timeout must be a non-negative integer, #f, or 'wait"}});
    }

    return std::chrono::milliseconds(numeric.int_val);
}

std::expected<actor::ActorSystem::MonitorRef, RuntimeError> decode_monitor_ref(
    Heap& heap,
    LispVal value,
    std::string_view op_name) {
    auto numeric = classify_numeric(value, heap);
    if (!numeric.is_valid() || !numeric.is_fixnum() || numeric.int_val < 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": expected non-negative monitor ref"}});
    }
    return static_cast<actor::ActorSystem::MonitorRef>(numeric.int_val);
}

std::expected<LispVal, RuntimeError> make_list(
    Heap& heap,
    std::span<const LispVal> values) {
    auto roots = heap.make_external_root_frame();
    for (const auto value : values) {
        roots.push(value);
    }

    LispVal out = nanbox::Nil;
    roots.push(out);
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        auto node = memory::factory::make_cons(heap, *it, out);
        if (!node.has_value()) return std::unexpected(node.error());
        out = *node;
        roots.push(out);
    }
    return out;
}

std::expected<LispVal, RuntimeError> decode_binary_payload(
    const actor::ActorSystem::BinaryMessage& payload,
    Heap& heap,
    InternTable& intern_table) {
    auto decoded = eta::nng::deserialize_binary(
        std::span<const std::uint8_t>(payload),
        heap,
        intern_table);
    if (!decoded.has_value()) return std::unexpected(decoded.error());
    return *decoded;
}

std::expected<LispVal, RuntimeError> decode_exit_reason(
    const actor::ActorSystem::ExitReason& reason,
    Heap& heap,
    InternTable& intern_table) {
    using ExitKind = actor::ActorSystem::ExitReason::Kind;
    switch (reason.kind) {
        case ExitKind::Normal:
            return memory::factory::make_symbol(intern_table, "normal");
        case ExitKind::Shutdown:
            return memory::factory::make_symbol(intern_table, "shutdown");
        case ExitKind::Killed:
            return memory::factory::make_symbol(intern_table, "killed");
        case ExitKind::Error:
            return memory::factory::make_symbol(intern_table, "error");
        case ExitKind::NoConnection:
            return memory::factory::make_symbol(intern_table, "noconnection");
        case ExitKind::BadCookie:
            return memory::factory::make_symbol(intern_table, "bad-cookie");
        case ExitKind::Custom:
            return decode_binary_payload(reason.payload, heap, intern_table);
    }

    return std::unexpected(RuntimeError{VMError{
        RuntimeErrorCode::InternalError,
        "actor exit reason decode failed"}});
}

std::expected<LispVal, RuntimeError> decode_actor_message(
    const actor::ActorSystem::Message& message,
    Heap& heap,
    InternTable& intern_table) {
    using MessageKind = actor::ActorSystem::Message::Kind;
    switch (message.kind) {
        case MessageKind::Payload:
            return decode_binary_payload(message.payload, heap, intern_table);

        case MessageKind::ExitSignal: {
            auto sym_exit = memory::factory::make_symbol(intern_table, "EXIT");
            if (!sym_exit.has_value()) return std::unexpected(sym_exit.error());
            auto from_pid = memory::factory::make_pid(heap, message.from);
            if (!from_pid.has_value()) return std::unexpected(from_pid.error());
            auto reason = decode_exit_reason(message.reason, heap, intern_table);
            if (!reason.has_value()) return std::unexpected(reason.error());
            std::array<LispVal, 3> values{*sym_exit, *from_pid, *reason};
            return make_list(heap, values);
        }

        case MessageKind::DownSignal: {
            auto sym_down = memory::factory::make_symbol(intern_table, "DOWN");
            if (!sym_down.has_value()) return std::unexpected(sym_down.error());
            auto ref = memory::factory::make_fixnum(
                heap,
                static_cast<std::int64_t>(message.monitor_ref));
            if (!ref.has_value()) return std::unexpected(ref.error());
            auto sym_process = memory::factory::make_symbol(intern_table, "process");
            if (!sym_process.has_value()) return std::unexpected(sym_process.error());
            auto pid = memory::factory::make_pid(heap, message.pid);
            if (!pid.has_value()) return std::unexpected(pid.error());
            auto reason = decode_exit_reason(message.reason, heap, intern_table);
            if (!reason.has_value()) return std::unexpected(reason.error());
            std::array<LispVal, 5> values{
                *sym_down,
                *ref,
                *sym_process,
                *pid,
                *reason};
            return make_list(heap, values);
        }

        case MessageKind::NodeUp: {
            auto sym_nodeup = memory::factory::make_symbol(intern_table, "nodeup");
            if (!sym_nodeup.has_value()) return std::unexpected(sym_nodeup.error());
            auto ref = memory::factory::make_fixnum(
                heap,
                static_cast<std::int64_t>(message.monitor_ref));
            if (!ref.has_value()) return std::unexpected(ref.error());
            auto node_name = memory::factory::make_string(
                heap,
                intern_table,
                message.node_name);
            if (!node_name.has_value()) return std::unexpected(node_name.error());
            auto node_id = memory::factory::make_fixnum(
                heap,
                static_cast<std::int64_t>(message.node_id));
            if (!node_id.has_value()) return std::unexpected(node_id.error());
            std::array<LispVal, 4> values{
                *sym_nodeup,
                *ref,
                *node_name,
                *node_id};
            return make_list(heap, values);
        }

        case MessageKind::NodeDown: {
            auto sym_nodedown = memory::factory::make_symbol(intern_table, "nodedown");
            if (!sym_nodedown.has_value()) return std::unexpected(sym_nodedown.error());
            auto ref = memory::factory::make_fixnum(
                heap,
                static_cast<std::int64_t>(message.monitor_ref));
            if (!ref.has_value()) return std::unexpected(ref.error());
            auto node_name = memory::factory::make_string(
                heap,
                intern_table,
                message.node_name);
            if (!node_name.has_value()) return std::unexpected(node_name.error());
            auto reason = decode_exit_reason(message.reason, heap, intern_table);
            if (!reason.has_value()) return std::unexpected(reason.error());
            std::array<LispVal, 4> values{
                *sym_nodedown,
                *ref,
                *node_name,
                *reason};
            return make_list(heap, values);
        }
    }

    return std::unexpected(RuntimeError{VMError{
        RuntimeErrorCode::InternalError,
        "actor message decode failed"}});
}

std::expected<LispVal, RuntimeError> encode_send_status(
    actor::ActorSystem::SendStatus status,
    InternTable& intern_table) {
    switch (status) {
        case actor::ActorSystem::SendStatus::Delivered:
            return memory::factory::make_symbol(intern_table, "ok");
        case actor::ActorSystem::SendStatus::NoSuchPid:
        case actor::ActorSystem::SendStatus::DeadPid:
            return memory::factory::make_symbol(intern_table, "no-process");
        case actor::ActorSystem::SendStatus::NoRoute:
            return memory::factory::make_symbol(intern_table, "no-route");
        case actor::ActorSystem::SendStatus::TransportError:
            return memory::factory::make_symbol(intern_table, "transport-error");
    }

    return std::unexpected(RuntimeError{VMError{
        RuntimeErrorCode::InternalError,
        "actor send status encode failed"}});
}

std::expected<DecodedExitSignal, RuntimeError> decode_exit_signal_reason(
    Heap& heap,
    InternTable& intern_table,
    LispVal reason_value,
    std::string_view op_name) {
    DecodedExitSignal out;

    if (symbol_eq(reason_value, intern_table, "normal")) {
        out.reason.kind = actor::ActorSystem::ExitReason::Kind::Normal;
        return out;
    }
    if (symbol_eq(reason_value, intern_table, "shutdown")) {
        out.reason.kind = actor::ActorSystem::ExitReason::Kind::Shutdown;
        return out;
    }
    if (symbol_eq(reason_value, intern_table, "killed")) {
        out.reason.kind = actor::ActorSystem::ExitReason::Kind::Killed;
        return out;
    }
    if (symbol_eq(reason_value, intern_table, "kill")) {
        out.reason.kind = actor::ActorSystem::ExitReason::Kind::Killed;
        out.untrappable = true;
        return out;
    }

    auto payload = eta::nng::serialize_binary_strict(reason_value, heap, intern_table);
    if (!payload.has_value()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": reason is not serializable: " + payload.error()}});
    }

    out.reason.kind = actor::ActorSystem::ExitReason::Kind::Custom;
    out.reason.payload = std::move(*payload);
    return out;
}

[[nodiscard]] std::int64_t saturating_fixnum_value(std::uint64_t value) {
    constexpr std::uint64_t max_fixnum =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (value > max_fixnum) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

std::expected<LispVal, RuntimeError> make_fixnum_u64(
    Heap& heap,
    std::uint64_t value) {
    return memory::factory::make_fixnum(heap, saturating_fixnum_value(value));
}

std::expected<LispVal, RuntimeError> encode_pid_list(
    const std::vector<types::Pid>& pids,
    Heap& heap) {
    auto roots = heap.make_external_root_frame();
    std::vector<LispVal> values;
    values.reserve(pids.size());

    for (const auto& pid : pids) {
        auto pid_value = memory::factory::make_pid(heap, pid);
        if (!pid_value.has_value()) return std::unexpected(pid_value.error());
        roots.push(*pid_value);
        values.push_back(*pid_value);
    }
    return make_list(heap, std::span<const LispVal>(values));
}

std::expected<LispVal, RuntimeError> encode_monitor_ref_list(
    const std::vector<actor::ActorSystem::MonitorRef>& refs,
    Heap& heap) {
    auto roots = heap.make_external_root_frame();
    std::vector<LispVal> values;
    values.reserve(refs.size());

    for (const auto ref : refs) {
        auto ref_value = make_fixnum_u64(heap, ref);
        if (!ref_value.has_value()) return std::unexpected(ref_value.error());
        roots.push(*ref_value);
        values.push_back(*ref_value);
    }
    return make_list(heap, std::span<const LispVal>(values));
}

std::expected<LispVal, RuntimeError> encode_yield_reason(
    actor::ActorSystem::YieldReason reason,
    InternTable& intern_table) {
    using YieldReason = actor::ActorSystem::YieldReason;
    switch (reason) {
        case YieldReason::None:
            return memory::factory::make_symbol(intern_table, "none");
        case YieldReason::BudgetExhausted:
            return memory::factory::make_symbol(intern_table, "budget-exhausted");
        case YieldReason::BlockedOnReceive:
            return memory::factory::make_symbol(intern_table, "blocked-on-receive");
        case YieldReason::Finished:
            return memory::factory::make_symbol(intern_table, "finished");
        case YieldReason::Error:
            return memory::factory::make_symbol(intern_table, "error");
    }
    return memory::factory::make_symbol(intern_table, "none");
}

std::expected<LispVal, RuntimeError> encode_run_state(
    actor::ActorSystem::RunState state,
    InternTable& intern_table) {
    using RunState = actor::ActorSystem::RunState;
    switch (state) {
        case RunState::Runnable:
            return memory::factory::make_symbol(intern_table, "runnable");
        case RunState::Running:
            return memory::factory::make_symbol(intern_table, "running");
        case RunState::Waiting:
            return memory::factory::make_symbol(intern_table, "waiting");
        case RunState::Exited:
            return memory::factory::make_symbol(intern_table, "exited");
    }
    return memory::factory::make_symbol(intern_table, "running");
}

std::expected<LispVal, RuntimeError> encode_process_info_value(
    const actor::ActorSystem::ProcessInfo& info,
    std::string_view key,
    Heap& heap,
    InternTable& intern_table) {
    if (key == "pid") {
        return memory::factory::make_pid(heap, info.pid);
    }
    if (key == "state") {
        return encode_run_state(info.run_state, intern_table);
    }
    if (key == "last-yield-reason") {
        return encode_yield_reason(info.last_yield_reason, intern_table);
    }
    if (key == "message-queue-len") {
        return make_fixnum_u64(heap, static_cast<std::uint64_t>(info.mailbox_length));
    }
    if (key == "registered-name") {
        if (info.registered_name.empty()) return nanbox::False;
        return memory::factory::make_symbol(intern_table, info.registered_name);
    }
    if (key == "links") {
        return encode_pid_list(info.links, heap);
    }
    if (key == "monitors") {
        return encode_monitor_ref_list(info.monitors, heap);
    }
    if (key == "reductions") {
        return make_fixnum_u64(heap, info.reductions);
    }
    return nanbox::False;
}

std::expected<LispVal, RuntimeError> encode_process_info_alist(
    const actor::ActorSystem::ProcessInfo& info,
    Heap& heap,
    InternTable& intern_table) {
    constexpr std::array<std::string_view, 8> kProcessInfoKeys{
        "pid",
        "state",
        "last-yield-reason",
        "message-queue-len",
        "registered-name",
        "links",
        "monitors",
        "reductions"};

    auto roots = heap.make_external_root_frame();
    std::vector<LispVal> entries;
    entries.reserve(kProcessInfoKeys.size());

    for (const auto key : kProcessInfoKeys) {
        auto value = encode_process_info_value(info, key, heap, intern_table);
        if (!value.has_value()) return std::unexpected(value.error());
        roots.push(*value);

        auto key_symbol = memory::factory::make_symbol(intern_table, std::string(key));
        if (!key_symbol.has_value()) return std::unexpected(key_symbol.error());
        roots.push(*key_symbol);

        auto pair = memory::factory::make_cons(heap, *key_symbol, *value);
        if (!pair.has_value()) return std::unexpected(pair.error());
        roots.push(*pair);
        entries.push_back(*pair);
    }

    return make_list(heap, std::span<const LispVal>(entries));
}

} // namespace

void PrimReg::register_actor_bridge() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;
    auto* vm = this->vm;

    env.register_builtin("%actor-self", 0, false,
        [&heap, vm](Args) -> std::expected<LispVal, RuntimeError> {
            auto pid = expect_current_pid(vm, "%actor-self");
            if (!pid.has_value()) return std::unexpected(pid.error());
            return memory::factory::make_pid(heap, *pid);
        });

    env.register_builtin("%actor-pid?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
                return nanbox::False;
            }
            return heap.try_get_as<ObjectKind::Pid, types::Pid>(ops::payload(args[0]))
                ? nanbox::True
                : nanbox::False;
        });

    env.register_builtin("%actor-alive?", 1, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-alive?: actor system is not available"}});
            }

            auto pid = expect_pid(heap, args[0], "%actor-alive?");
            if (!pid.has_value()) return std::unexpected(pid.error());
            return vm->actor_system()->pid_exists(*pid)
                ? nanbox::True
                : nanbox::False;
        });

    env.register_builtin("%actor-spawn", 1, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-spawn: VM context is unavailable"}});
            }

            auto pid = vm->spawn_actor(args[0]);
            if (!pid.has_value()) return std::unexpected(pid.error());
            return memory::factory::make_pid(heap, *pid);
        });

    env.register_builtin("%actor-send", 2, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-send: actor system is not available"}});
            }

            types::Pid target_pid{};
            if (auto pid = try_decode_pid(heap, args[0]); pid.has_value()) {
                target_pid = *pid;
            } else {
                auto name = decode_registry_name(intern_table, args[0], "%actor-send");
                if (!name.has_value()) return std::unexpected(name.error());
                auto resolved = vm->actor_system()->whereis(*name);
                if (!resolved.has_value()) return nanbox::False;
                target_pid = *resolved;
            }

            auto payload = eta::nng::serialize_binary_strict(args[1], heap, intern_table);
            if (!payload.has_value()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "%actor-send: payload is not serializable: " + payload.error()}});
            }

            const bool sent = vm->actor_system()->send(target_pid, std::move(*payload));
            return sent ? args[1] : nanbox::False;
        });

    env.register_builtin("%actor-send-checked", 2, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-send-checked: actor system is not available"}});
            }

            types::Pid target_pid{};
            if (auto pid = try_decode_pid(heap, args[0]); pid.has_value()) {
                target_pid = *pid;
            } else {
                auto name = decode_registry_name(intern_table, args[0], "%actor-send-checked");
                if (!name.has_value()) return std::unexpected(name.error());
                auto resolved = vm->actor_system()->whereis(*name);
                if (!resolved.has_value()) {
                    auto status = memory::factory::make_symbol(intern_table, "no-process");
                    if (!status.has_value()) return std::unexpected(status.error());
                    return *status;
                }
                target_pid = *resolved;
            }

            auto payload = eta::nng::serialize_binary_strict(args[1], heap, intern_table);
            if (!payload.has_value()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "%actor-send-checked: payload is not serializable: " + payload.error()}});
            }

            const auto status = vm->actor_system()->send_checked(target_pid, std::move(*payload));
            auto encoded = encode_send_status(status, intern_table);
            if (!encoded.has_value()) return std::unexpected(encoded.error());
            return *encoded;
        });

    env.register_builtin("%actor-receive", 2, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-receive: actor system is not available"}});
            }

            auto pid = expect_current_pid(vm, "%actor-receive");
            if (!pid.has_value()) return std::unexpected(pid.error());

            auto timeout = decode_receive_timeout(heap, intern_table, args[1]);
            if (!timeout.has_value()) return std::unexpected(timeout.error());

            std::optional<actor::ActorSystem::Message> payload;
            if (args[0] == nanbox::False) {
                payload = vm->actor_system()->receive(*pid, *timeout);
            } else {
                auto matched_payload = vm->actor_system()->receive_matching(
                    *pid,
                    *timeout,
                    [&heap, &intern_table, vm, matcher = args[0]](
                        const actor::ActorSystem::Message& candidate)
                        -> std::expected<bool, RuntimeError> {
                        auto decoded = decode_actor_message(candidate, heap, intern_table);
                        if (!decoded.has_value()) return std::unexpected(decoded.error());

                        auto roots = heap.make_external_root_frame();
                        roots.push(matcher);
                        roots.push(*decoded);
                        auto matched = vm->call_value(matcher, {*decoded});
                        if (!matched.has_value()) return std::unexpected(matched.error());
                        return *matched != nanbox::False;
                    });
                if (!matched_payload.has_value()) {
                    return std::unexpected(matched_payload.error());
                }
                payload = *matched_payload;
            }

            if (!payload.has_value()) return nanbox::False;

            auto decoded = decode_actor_message(*payload, heap, intern_table);
            if (!decoded.has_value()) return std::unexpected(decoded.error());
            return *decoded;
        });

    env.register_builtin("%actor-mailbox-len", 0, false,
        [&heap, vm](Args) -> std::expected<LispVal, RuntimeError> {
            auto pid = expect_current_pid(vm, "%actor-mailbox-len");
            if (!pid.has_value()) return std::unexpected(pid.error());

            auto len = vm->actor_system()->mailbox_size(*pid);
            if (!len.has_value()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-mailbox-len: mailbox is unavailable"}});
            }

            return memory::factory::make_fixnum(heap, static_cast<std::int64_t>(*len));
        });

    env.register_builtin("%actor-process-info", 1, true,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-process-info: actor system is not available"}});
            }

            if (args.size() > 2) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InvalidArity,
                    "%actor-process-info: expected 1 or 2 arguments"}});
            }

            auto pid = expect_pid(heap, args[0], "%actor-process-info");
            if (!pid.has_value()) return std::unexpected(pid.error());

            auto info = vm->actor_system()->process_info(*pid);
            if (!info.has_value()) return nanbox::False;

            if (args.size() == 1) {
                auto encoded = encode_process_info_alist(*info, heap, intern_table);
                if (!encoded.has_value()) return std::unexpected(encoded.error());
                return *encoded;
            }

            auto key = decode_registry_name(intern_table, args[1], "%actor-process-info");
            if (!key.has_value()) return std::unexpected(key.error());

            auto value = encode_process_info_value(*info, *key, heap, intern_table);
            if (!value.has_value()) return std::unexpected(value.error());
            return *value;
        });

    env.register_builtin("%actor-trap-exit!", 1, false,
        [vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto pid = expect_current_pid(vm, "%actor-trap-exit!");
            if (!pid.has_value()) return std::unexpected(pid.error());

            const bool enabled = args[0] != nanbox::False;
            const bool ok = vm->actor_system()->set_trap_exit(*pid, enabled);
            if (!ok) return nanbox::False;
            return enabled ? nanbox::True : nanbox::False;
        });

    env.register_builtin("%actor-link", 1, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto self = expect_current_pid(vm, "%actor-link");
            if (!self.has_value()) return std::unexpected(self.error());

            auto target = expect_pid(heap, args[0], "%actor-link");
            if (!target.has_value()) return std::unexpected(target.error());

            return vm->actor_system()->link(*self, *target)
                ? nanbox::True
                : nanbox::False;
        });

    env.register_builtin("%actor-unlink", 1, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto self = expect_current_pid(vm, "%actor-unlink");
            if (!self.has_value()) return std::unexpected(self.error());

            auto target = expect_pid(heap, args[0], "%actor-unlink");
            if (!target.has_value()) return std::unexpected(target.error());

            return vm->actor_system()->unlink(*self, *target)
                ? nanbox::True
                : nanbox::False;
        });

    env.register_builtin("%actor-monitor", 1, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto watcher = expect_current_pid(vm, "%actor-monitor");
            if (!watcher.has_value()) return std::unexpected(watcher.error());

            auto target = expect_pid(heap, args[0], "%actor-monitor");
            if (!target.has_value()) return std::unexpected(target.error());

            auto ref = vm->actor_system()->monitor(*watcher, *target);
            if (!ref.has_value()) return nanbox::False;
            return memory::factory::make_fixnum(heap, static_cast<std::int64_t>(*ref));
        });

    env.register_builtin("%actor-demonitor", 1, true,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto watcher = expect_current_pid(vm, "%actor-demonitor");
            if (!watcher.has_value()) return std::unexpected(watcher.error());

            auto ref = decode_monitor_ref(heap, args[0], "%actor-demonitor");
            if (!ref.has_value()) return std::unexpected(ref.error());

            const bool flush = args.size() > 1 && args[1] != nanbox::False;
            const bool ok = vm->actor_system()->demonitor(*watcher, *ref, flush);
            return ok ? nanbox::True : nanbox::False;
        });

    env.register_builtin("%actor-exit", 2, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto from = expect_current_pid(vm, "%actor-exit");
            if (!from.has_value()) return std::unexpected(from.error());

            auto target = expect_pid(heap, args[0], "%actor-exit");
            if (!target.has_value()) return std::unexpected(target.error());

            auto reason = decode_exit_signal_reason(
                heap,
                intern_table,
                args[1],
                "%actor-exit");
            if (!reason.has_value()) return std::unexpected(reason.error());

            const bool ok = vm->actor_system()->signal_exit(
                *from,
                *target,
                std::move(reason->reason),
                reason->untrappable);
            return ok ? nanbox::True : nanbox::False;
        });

    env.register_builtin("%actor-kill", 1, true,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto from = expect_current_pid(vm, "%actor-kill");
            if (!from.has_value()) return std::unexpected(from.error());

            auto target = expect_pid(heap, args[0], "%actor-kill");
            if (!target.has_value()) return std::unexpected(target.error());

            actor::ActorSystem::ExitReason reason;
            reason.kind = actor::ActorSystem::ExitReason::Kind::Killed;
            const bool ok = vm->actor_system()->signal_exit(
                *from,
                *target,
                std::move(reason),
                true);
            return ok ? nanbox::True : nanbox::False;
        });

    env.register_builtin("%actor-register", 2, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-register: actor system is not available"}});
            }

            auto name = decode_registry_name(intern_table, args[0], "%actor-register");
            if (!name.has_value()) return std::unexpected(name.error());

            auto pid = expect_pid(heap, args[1], "%actor-register");
            if (!pid.has_value()) return std::unexpected(pid.error());

            const bool ok = vm->actor_system()->register_name(std::move(*name), *pid);
            return ok ? nanbox::True : nanbox::False;
        });

    env.register_builtin("%actor-unregister", 1, false,
        [&intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-unregister: actor system is not available"}});
            }

            auto name = decode_registry_name(intern_table, args[0], "%actor-unregister");
            if (!name.has_value()) return std::unexpected(name.error());

            const bool ok = vm->actor_system()->unregister_name(*name);
            return ok ? nanbox::True : nanbox::False;
        });

    env.register_builtin("%actor-whereis", 1, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-whereis: actor system is not available"}});
            }

            auto name = decode_registry_name(intern_table, args[0], "%actor-whereis");
            if (!name.has_value()) return std::unexpected(name.error());

            auto pid = vm->actor_system()->whereis(*name);
            if (!pid.has_value()) return nanbox::False;
            return memory::factory::make_pid(heap, *pid);
        });

    env.register_builtin("%actor-registered", 0, false,
        [&heap, &intern_table, vm](Args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-registered: actor system is not available"}});
            }

            const auto names = vm->actor_system()->registered_names();
            std::vector<LispVal> symbols;
            symbols.reserve(names.size());

            for (const auto& name : names) {
                auto symbol = memory::factory::make_symbol(intern_table, name);
                if (!symbol.has_value()) return std::unexpected(symbol.error());
                symbols.push_back(*symbol);
            }

            return make_list(heap, std::span<const LispVal>(symbols));
        });

    env.register_builtin("%actor-node-name", 0, false,
        [&heap, &intern_table, vm](Args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-node-name: actor system is not available"}});
            }

            return memory::factory::make_string(
                heap,
                intern_table,
                vm->actor_system()->node_name());
        });

    env.register_builtin("%actor-monitor-node", 1, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-monitor-node: actor system is not available"}});
            }

            auto watcher = expect_current_pid(vm, "%actor-monitor-node");
            if (!watcher.has_value()) return std::unexpected(watcher.error());

            auto node_name = decode_text_value(
                intern_table,
                args[0],
                "%actor-monitor-node",
                "node name");
            if (!node_name.has_value()) return std::unexpected(node_name.error());

            auto ref = vm->actor_system()->monitor_node(*watcher, *node_name);
            if (!ref.has_value()) return nanbox::False;
            return memory::factory::make_fixnum(heap, static_cast<std::int64_t>(*ref));
        });

    env.register_builtin("%actor-node-listen", 1, true,
        [&intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-node-listen: actor system is not available"}});
            }

            auto endpoint = decode_text_value(
                intern_table,
                args[0],
                "%actor-node-listen",
                "endpoint");
            if (!endpoint.has_value()) return std::unexpected(endpoint.error());

            auto options = decode_node_options(
                intern_table,
                args,
                1u,
                "%actor-node-listen");
            if (!options.has_value()) return std::unexpected(options.error());

            if (options->node_name.has_value()) {
                std::string configure_error;
                if (!vm->actor_system()->configure_node(
                        *options->node_name,
                        *options->cookie,
                        &configure_error)) {
                    return nanbox::False;
                }
            }

            std::string error_message;
            const bool ok = vm->actor_system()->node_listen(
                *endpoint,
                &error_message);
            return ok ? nanbox::True : nanbox::False;
        });

    env.register_builtin("%actor-node-connect", 1, true,
        [&intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-node-connect: actor system is not available"}});
            }

            auto endpoint = decode_text_value(
                intern_table,
                args[0],
                "%actor-node-connect",
                "endpoint");
            if (!endpoint.has_value()) return std::unexpected(endpoint.error());

            auto options = decode_node_options(
                intern_table,
                args,
                1u,
                "%actor-node-connect");
            if (!options.has_value()) return std::unexpected(options.error());

            if (options->node_name.has_value()) {
                std::string configure_error;
                if (!vm->actor_system()->configure_node(
                        *options->node_name,
                        *options->cookie,
                        &configure_error)) {
                    return nanbox::False;
                }
            }

            std::string error_message;
            const bool ok = vm->actor_system()->node_connect(
                *endpoint,
                &error_message);
            return ok ? nanbox::True : nanbox::False;
        });

    env.register_builtin("%actor-nodes", 0, false,
        [&heap, &intern_table, vm](Args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-nodes: actor system is not available"}});
            }

            const auto nodes = vm->actor_system()->connected_nodes();
            auto roots = heap.make_external_root_frame();
            std::vector<LispVal> entries;
            entries.reserve(nodes.size());

            for (const auto& node : nodes) {
                auto name = memory::factory::make_string(
                    heap,
                    intern_table,
                    node.node_name);
                if (!name.has_value()) return std::unexpected(name.error());
                roots.push(*name);

                auto node_id = memory::factory::make_fixnum(
                    heap,
                    static_cast<std::int64_t>(node.node_id));
                if (!node_id.has_value()) return std::unexpected(node_id.error());
                roots.push(*node_id);

                auto endpoint = memory::factory::make_string(
                    heap,
                    intern_table,
                    node.endpoint);
                if (!endpoint.has_value()) return std::unexpected(endpoint.error());
                roots.push(*endpoint);

                std::array<LispVal, 3> node_values{*name, *node_id, *endpoint};
                auto node_entry = make_list(heap, node_values);
                if (!node_entry.has_value()) return std::unexpected(node_entry.error());
                roots.push(*node_entry);
                entries.push_back(*node_entry);
            }

            return make_list(heap, std::span<const LispVal>(entries));
        });

    env.register_builtin("%actor-disconnect-node", 1, false,
        [&intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm || !vm->actor_system()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "%actor-disconnect-node: actor system is not available"}});
            }

            auto node_name = decode_text_value(
                intern_table,
                args[0],
                "%actor-disconnect-node",
                "node name");
            if (!node_name.has_value()) return std::unexpected(node_name.error());

            return vm->actor_system()->disconnect_node(*node_name)
                ? nanbox::True
                : nanbox::False;
        });
}

} // namespace eta::runtime
