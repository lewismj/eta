/// DAP threads, stack frames, scopes and variables (inspection and setVariable).
#include "dap_server.h"
#include "dap_io.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

/// Eta interpreter
#include "eta/session/driver.h"
#include "eta/session/bootstrap_module.h"
#include "eta/interpreter/module_path.h"
#include "eta/docs/markdown.h"
#include "eta/docs/stdlib_docs.h"
#include "eta/reader/special_form_docs.h"
#include "eta/runtime/builtin_metadata.h"
#include "eta/runtime/port.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/vm/disassembler.h"
#include "eta/runtime/vm/sandbox.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/actor/actor_system.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/memory/cons_pool.h"
#include "eta/runtime/memory/native_object_inspection.h"
#include "eta/runtime/types/types.h"
#include "eta/diagnostic/diagnostic.h"

#ifdef ETA_HAS_NNG
#include "eta/nng/nng_socket_ptr.h"
#include "eta/nng/nng_primitives.h"
#include "dap_server_internal.h"
#endif

namespace eta::dap {

namespace fs = std::filesystem;
using namespace eta::json;

/**
 * threads
 */

void DapServer::handle_threads(const Value& id, const Value& /*args*/) {
    Array threads;
    std::lock_guard<std::mutex> lk(vm_mutex_);
    if (threads_by_id_.empty()) {
        /// Even without a launch we always advertise a main thread so VS Code
        /// can render an empty Call Stack pane without a "No threads" warning.
        threads.push_back(json::object({{"id", 1}, {"name", "main"}}));
    } else {
        /// Emit threads in id order so the IDE displays them stably.
        std::vector<int> ids;
        ids.reserve(threads_by_id_.size());
        for (const auto& [id_k, _] : threads_by_id_) ids.push_back(id_k);
        std::sort(ids.begin(), ids.end());
        for (int tid : ids) {
            const auto& th = threads_by_id_.at(tid);
            threads.push_back(json::object({
                {"id",   Value(static_cast<int64_t>(th.dap_thread_id))},
                {"name", Value(th.name)},
            }));
        }
    }
    send_response(id, json::object({{"threads", Value(std::move(threads))}}));
}

/**
 * stackTrace
 */

void DapServer::handle_stack_trace(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);
    DapThread* th = resolve_thread_arg_locked(args);
    if (!th || !th->vm || !th->vm->is_paused()) {
        send_response(id, json::object({{"stackFrames", json::array({})}, {"totalFrames", 0}}));
        return;
    }

    session::Driver& drv = *th->driver;
    auto frames = th->vm->get_frames();
    Array frames_arr;
    int frame_idx = 0;
    /// Encode threadId into frameId so subsequent scopes / evaluate requests
    /// can route back to the correct VM (DAP only carries frameId, not
    /// threadId, in those follow-up calls).
    const int tid_high = (th->dap_thread_id & 0xFFF) << 16;
    for (const auto& fi : frames) {
        const auto& sp = fi.span;
        Value source_val = source_json_for(drv, sp.file_id);
        const uint32_t line = std::max<uint32_t>(1u, sp.start.line);
        const uint32_t column = std::max<uint32_t>(1u, sp.start.column);

        const int frame_id = tid_high | (frame_idx & 0xFFFF);
        frames_arr.push_back(json::object({
            {"id",     Value(static_cast<int64_t>(frame_id))},
            {"name",   fi.func_name.empty() ? Value("<anonymous>") : Value(fi.func_name)},
            {"source", source_val},
            {"line",   Value(static_cast<int64_t>(line))},
            {"column", Value(static_cast<int64_t>(column))},
        }));
        ++frame_idx;
    }

    send_response(id, json::object({
        {"stackFrames", Value(std::move(frames_arr))},
        {"totalFrames", Value(static_cast<int64_t>(frame_idx))},
    }));
}

/**
 * scopes
 */

void DapServer::handle_scopes(const Value& id, const Value& args) {
    auto frame_id_opt = args.get_int("frameId");
    int frame_id = frame_id_opt ? static_cast<int>(*frame_id_opt) : 0;

    /// Frame ids carry the thread id in their high bits (see handle_stack_trace).
    int thread_id = (frame_id >> 16) & 0xFFF;
    if (thread_id == 0) thread_id = MAIN_THREAD_ID;
    int frame_idx = frame_id & 0xFFFF;

    send_response(id, json::object({
        {"scopes", json::array({
            json::object({
                {"name",               "Module"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(thread_id, frame_idx, 3)))},
                {"expensive",          false},
                {"presentationHint",   "locals"},
            }),
            json::object({
                {"name",               "Locals"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(thread_id, frame_idx, 0)))},
                {"expensive",          false},
            }),
            json::object({
                {"name",               "Upvalues"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(thread_id, frame_idx, 1)))},
                {"expensive",          false},
            }),
            json::object({
                {"name",               "Globals"},
                {"variablesReference", Value(static_cast<int64_t>(encode_var_ref(thread_id, frame_idx, 2)))},
                {"expensive",          true},
            }),
        })},
    }));
}

/**
 * variables
 */

void DapServer::handle_variables(const Value& id, const Value& args) {
    std::lock_guard<std::mutex> lk(vm_mutex_);

    auto ref_opt = args.get_int("variablesReference");
    if (!ref_opt) {
        send_response(id, json::object({{"variables", json::array({})}}));
        return;
    }
    int ref = static_cast<int>(*ref_opt);

    /// Compound variable expansion (cons/vector/closure)  -  owning thread is
    /// recorded in the CompoundRef so we look up the right Driver/heap.
    if (ref >= COMPOUND_REF_BASE) {
        auto cit = compound_refs_.find(ref);
        if (cit == compound_refs_.end()) {
            send_response(id, json::object({{"variables", json::array({})}}));
            return;
        }
        DapThread* th = find_thread_locked(cit->second.dap_thread_id);
        if (!th || !th->driver) {
            send_response(id, json::object({{"variables", json::array({})}}));
            return;
        }
        const auto start_opt = args.get_int("start");
        const auto count_opt = args.get_int("count");
        const int  start = start_opt ? static_cast<int>(*start_opt) : 0;
        const int  count = count_opt ? static_cast<int>(*count_opt) : 0;
        auto children = expand_compound(*th->driver, th->dap_thread_id, cit->second.value, start, count);
        send_response(id, json::object({{"variables", Value(std::move(children))}}));
        return;
    }

    /// Frame scope variables (locals / upvalues / globals).
    int thread_id = decode_var_ref_thread(ref);
    int frame_idx = decode_var_ref_frame(ref);
    int scope     = decode_var_ref_scope(ref);

    DapThread* th = find_thread_locked(thread_id);
    if (!th || !th->vm || !th->driver || !th->vm->is_paused()) {
        send_response(id, json::object({{"variables", json::array({})}}));
        return;
    }
    session::Driver& drv = *th->driver;
    runtime::vm::VM& vm = *th->vm;

    if (scope == 3) {
        /**
         * Module scope: globals that belong to the currently-executing module
         * so they are immediately visible without scrolling through all prelude/std.* entries.
         */
        std::string cur_mod = current_module_from_frame(drv, static_cast<std::size_t>(frame_idx));
        const auto& globals = vm.globals();
        const auto& names   = drv.global_names();
        Array vars;
        for (const auto& [slot, full_name] : names) {
            if (slot >= globals.size()) continue;
            auto v = globals[slot];
            if (v == runtime::nanbox::Nil) continue;
            auto dot = full_name.rfind('.');
            if (dot == std::string::npos) continue;
            if (full_name.substr(0, dot) != cur_mod) continue;
            vars.push_back(make_variable_json(drv, thread_id, full_name.substr(dot + 1), v));
        }
        send_response(id, json::object({{"variables", Value(std::move(vars))}}));
        return;
    }

    if (scope == 2) {
        /// Globals scope
        const auto& globals = vm.globals();
        const auto& names   = drv.global_names();
        Array vars;
        for (std::size_t slot = 0; slot < globals.size(); ++slot) {
            auto v = globals[slot];
            if (v == runtime::nanbox::Nil) continue;

            auto it = names.find(static_cast<uint32_t>(slot));
            std::string name = (it != names.end()) ? it->second
                                                   : "global[" + std::to_string(slot) + "]";
            vars.push_back(make_variable_json(drv, thread_id, name, v));
        }
        send_response(id, json::object({{"variables", Value(std::move(vars))}}));
        return;
    }

    auto entries = (scope == 0)
                   ? vm.get_locals(static_cast<std::size_t>(frame_idx))
                   : vm.get_upvalues(static_cast<std::size_t>(frame_idx));

    Array vars;
    for (const auto& e : entries) {
        if (!e.name.empty() && e.name[0] == '%' && e.value == runtime::nanbox::Nil)
            continue;
        vars.push_back(make_variable_json(drv, thread_id, e.name, e.value));
    }

    send_response(id, json::object({{"variables", Value(std::move(vars))}}));
}

/**
 * setVariable
 */

void DapServer::handle_set_variable(const Value& id, const Value& args) {
    const auto ref_opt = args.get_int("variablesReference");
    const auto name_opt = args.get_string("name");
    const auto value_opt = args.get_string("value");

    if (!ref_opt || !name_opt || !value_opt) {
        send_error_response(id, 2010, "setVariable requires variablesReference, name, and value");
        return;
    }

    const int ref = static_cast<int>(*ref_opt);
    if (ref <= 0 || ref >= COMPOUND_REF_BASE) {
        send_error_response(id, 2011, "setVariable only supports locals/module/globals scopes");
        return;
    }

    std::lock_guard<std::mutex> lk(vm_mutex_);
    int thread_id = decode_var_ref_thread(ref);
    DapThread* th = find_thread_locked(thread_id);
    if (!th || !th->driver || !th->vm) {
        send_error_response(id, 2001, "VM not running");
        return;
    }
    if (!th->vm->is_paused()) {
        send_error_response(id, 2002, "VM must be paused to set variable values");
        return;
    }
    session::Driver& drv = *th->driver;
    runtime::vm::VM& vm = *th->vm;

    uint64_t new_value = runtime::nanbox::Nil;
    std::string parse_error;
    if (!parse_set_variable_value(drv, *value_opt, new_value, parse_error)) {
        send_error_response(id, 2012, "setVariable: " + parse_error);
        return;
    }

    const int frame_idx = decode_var_ref_frame(ref);
    const int scope = decode_var_ref_scope(ref);

    const std::string requested_name = *name_opt;
    std::optional<uint32_t> global_slot;

    auto resolve_global_slot = [&](bool module_scope) -> std::optional<uint32_t> {
        const auto& names = drv.global_names();
        const auto& globals = vm.globals();

        auto exact_full = [&](const std::string& full_name) -> std::optional<uint32_t> {
            for (const auto& [slot, name] : names) {
                if (slot < globals.size() && name == full_name) return slot;
            }
            return std::nullopt;
        };

        if (module_scope) {
            const std::string cur_mod = current_module_from_frame(drv, static_cast<std::size_t>(frame_idx));
            if (cur_mod.empty()) return std::nullopt;
            if (requested_name.find('.') != std::string::npos) {
                return exact_full(requested_name);
            }
            return exact_full(cur_mod + "." + requested_name);
        }

        if (requested_name.find('.') != std::string::npos) {
            return exact_full(requested_name);
        }

        std::optional<uint32_t> found;
        for (const auto& [slot, full_name] : names) {
            if (slot >= globals.size()) continue;
            const auto dot = full_name.rfind('.');
            const std::string short_name = (dot == std::string::npos)
                ? full_name
                : full_name.substr(dot + 1);
            if (short_name != requested_name) continue;
            if (found.has_value()) return std::nullopt; ///< ambiguous short name
            found = slot;
        }
        return found;
    };

    switch (scope) {
        case 0: {
            const auto locals = vm.get_locals(static_cast<std::size_t>(frame_idx));
            std::optional<std::size_t> slot;
            for (std::size_t i = 0; i < locals.size(); ++i) {
                if (locals[i].name == requested_name) {
                    slot = i;
                    break;
                }
            }
            if (!slot.has_value()) {
                send_error_response(id, 2013, "setVariable: local not found: " + requested_name);
                return;
            }
            if (!vm.set_local(static_cast<std::size_t>(frame_idx), *slot, new_value)) {
                send_error_response(id, 2014, "setVariable: failed to update local slot");
                return;
            }
            break;
        }
        case 1:
            send_error_response(id, 2015, "setVariable: upvalues are read-only");
            return;
        case 2:
            global_slot = resolve_global_slot(false);
            break;
        case 3:
            global_slot = resolve_global_slot(true);
            break;
        default:
            send_error_response(id, 2016, "setVariable: unsupported scope");
            return;
    }

    if ((scope == 2 || scope == 3)) {
        if (!global_slot.has_value()) {
            send_error_response(id, 2017, "setVariable: global not found (or ambiguous): " + requested_name);
            return;
        }
        auto& globals = vm.globals();
        if (*global_slot >= globals.size()) {
            send_error_response(id, 2018, "setVariable: resolved global slot is out of range");
            return;
        }
        globals[*global_slot] = new_value;
    }

    const int vr = is_compound_value(new_value) ? alloc_compound_ref(new_value, thread_id) : 0;
    send_response(id, json::object({
        {"value",              Value(drv.format_value(new_value))},
        {"variablesReference", Value(static_cast<int64_t>(vr))},
    }));
}

bool DapServer::parse_set_variable_value(session::Driver& drv,
                                         const std::string& value_text,
                                         uint64_t& out_value,
                                         std::string& out_error) {
    out_error.clear();
    const std::string text = trim_copy(value_text);
    if (text.empty()) {
        out_error = "value must not be empty";
        return false;
    }

    if (text == "#t" || iequals(text, "true")) {
        out_value = runtime::nanbox::True;
        return true;
    }
    if (text == "#f" || iequals(text, "false")) {
        out_value = runtime::nanbox::False;
        return true;
    }
    if (text == "nil" || text == "()") {
        out_value = runtime::nanbox::Nil;
        return true;
    }

    int64_t as_int = 0;
    if (try_parse_i64(text, as_int)) {
        auto enc = runtime::nanbox::ops::encode<int64_t>(as_int);
        if (!enc) {
            out_error = "integer out of range for Eta fixnum";
            return false;
        }
        out_value = *enc;
        return true;
    }

    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        const std::string raw = text.substr(1, text.size() - 2);
        auto sid = drv.intern_table().intern(raw);
        if (!sid) {
            out_error = "failed to intern string value";
            return false;
        }
        out_value = runtime::nanbox::ops::box(runtime::nanbox::Tag::String, *sid);
        return true;
    }

    if (text.find('.') != std::string::npos ||
        text.find('e') != std::string::npos ||
        text.find('E') != std::string::npos) {
        char* end = nullptr;
        const double as_double = std::strtod(text.c_str(), &end);
        if (end != text.c_str() && end != nullptr && *end == '\0') {
            auto enc = runtime::nanbox::ops::encode<double>(as_double);
            if (!enc) {
                out_error = "invalid floating-point value";
                return false;
            }
            out_value = *enc;
            return true;
        }
    }

    if (is_identifier_expr(text) && try_lookup_paused_name(drv, text, out_value)) {
        return true;
    }

    {
        std::string formatted;
        std::string sandbox_err;
        bool        violation = false;
        if (eval_in_paused_frame(drv, 0, text, formatted, &out_value, &sandbox_err, &violation)) {
            return true;
        }
        out_error = violation
            ? "sandbox-violation: " + sandbox_err
            : sandbox_err;
        return false;
    }
}

/**
 * Compound variable expansion helpers
 */

int DapServer::alloc_compound_ref(uint64_t val, int dap_thread_id) {
    int ref = next_compound_ref_++;
    compound_refs_[ref] = CompoundRef{val, dap_thread_id};
    return ref;
}

void DapServer::clear_compound_refs() {
    compound_refs_.clear();
    next_compound_ref_ = COMPOUND_REF_BASE;
}

bool DapServer::is_compound_value(uint64_t val) const {
    using namespace runtime::nanbox;
    if (!ops::is_boxed(val) || ops::tag(val) != Tag::HeapObject) return false;
    if (!driver_) return false;
    auto& heap = driver_->heap();
    runtime::memory::heap::HeapEntry entry;
    if (!heap.try_get(static_cast<runtime::memory::heap::ObjectId>(ops::payload(val)), entry))
        return false;
    switch (entry.header.kind) {
        case runtime::memory::heap::ObjectKind::Cons:
        case runtime::memory::heap::ObjectKind::Vector:
        case runtime::memory::heap::ObjectKind::Closure:
#ifdef ETA_HAS_NNG
        case runtime::memory::heap::ObjectKind::NngSocket:
#endif
            return true;
        default:
            return false;
    }
}

namespace {

std::string heap_kind_type_hint(runtime::memory::heap::ObjectKind kind) {
    using runtime::memory::heap::ObjectKind;
    switch (kind) {
        case ObjectKind::Cons:
            return "pair";
        case ObjectKind::Fixnum:
            return "integer";
        case ObjectKind::Vector:
            return "vector";
        case ObjectKind::ByteVector:
            return "bytevector";
        case ObjectKind::Closure:
            return "procedure";
        case ObjectKind::Continuation:
            return "continuation";
        case ObjectKind::Primitive:
            return "builtin";
        case ObjectKind::Guardian:
            return "guardian";
        case ObjectKind::MultipleValues:
            return "multiple-values";
        case ObjectKind::Port:
            return "port";
        case ObjectKind::LogicVar:
            return "logic-var";
        case ObjectKind::Tape:
            return "tape";
        case ObjectKind::Tensor:
            return "tensor";
        case ObjectKind::NNModule:
            return "nn-module";
        case ObjectKind::Optimizer:
            return "optimizer";
        case ObjectKind::FactTable:
            return "fact-table";
        case ObjectKind::HashMap:
            return "hashmap";
        case ObjectKind::HashSet:
            return "hashset";
        case ObjectKind::CsvReader:
            return "csv-reader";
        case ObjectKind::CsvWriter:
            return "csv-writer";
        case ObjectKind::Regex:
            return "regex";
        case ObjectKind::LogSink:
            return "log-sink";
        case ObjectKind::LogLogger:
            return "log-logger";
        case ObjectKind::ProcessHandle:
            return "process";
        case ObjectKind::NngSocket:
            return "socket";
        case ObjectKind::CompoundTerm:
            return "compound-term";
        case ObjectKind::NativeObject:
            return "native-object";
        default:
            return "object";
    }
}

std::string classify_value_type(runtime::memory::heap::Heap& heap, runtime::nanbox::LispVal val) {
    using namespace runtime::nanbox;
    if (!ops::is_boxed(val)) {
        return "number";
    }
    switch (ops::tag(val)) {
        case Tag::Nil:
            if (val == runtime::nanbox::True || val == runtime::nanbox::False) {
                return "boolean";
            }
            return "nil";
        case Tag::Char:
            return "char";
        case Tag::Fixnum:
            return "integer";
        case Tag::String:
            return "string";
        case Tag::Symbol:
            return "symbol";
        case Tag::Nan:
            return "number";
        case Tag::TapeRef:
            return "tape-ref";
        case Tag::HeapObject: {
            runtime::memory::heap::HeapEntry entry;
            if (heap.try_get(static_cast<runtime::memory::heap::ObjectId>(ops::payload(val)), entry)) {
                return heap_kind_type_hint(entry.header.kind);
            }
            return "object";
        }
        default:
            return "value";
    }
}

bool is_callable_type_hint(const std::string& type_hint) {
    return type_hint == "procedure"
        || type_hint == "builtin"
        || type_hint == "continuation";
}

} // namespace

Value DapServer::make_variable_json(session::Driver& drv, int dap_thread_id,
                                    const std::string& name, uint64_t val) {
    using namespace runtime::nanbox;
    using namespace runtime::memory::heap;
    using namespace runtime::types;

    int var_ref = 0;
    int64_t indexed_count = 0;
    int64_t named_count   = 0;
    std::optional<int64_t> object_id;
    /// is_compound_value() probes the *parent* heap; for child threads we
    /// re-probe against `drv.heap()` directly to determine expandability.
    auto& heap = drv.heap();
    if (ops::is_boxed(val) && ops::tag(val) == Tag::HeapObject) {
        const auto pid = static_cast<ObjectId>(ops::payload(val));
        HeapEntry entry;
        if (heap.try_get(pid, entry)) {
            object_id = static_cast<int64_t>(pid);
            switch (entry.header.kind) {
                case ObjectKind::Vector:
                    if (auto* vec = heap.try_get_as<ObjectKind::Vector, Vector>(pid)) {
                        var_ref = alloc_compound_ref(val, dap_thread_id);
                        indexed_count = static_cast<int64_t>(vec->elements.size());
                    }
                    break;
                case ObjectKind::Cons:
                    var_ref = alloc_compound_ref(val, dap_thread_id);
                    named_count = 2;
                    break;
                case ObjectKind::Closure:
                    if (auto* cl = heap.try_get_as<ObjectKind::Closure, Closure>(pid)) {
                        var_ref = alloc_compound_ref(val, dap_thread_id);
                        named_count = static_cast<int64_t>(cl->upvals.size()) + (cl->func ? 1 : 0);
                    }
                    break;
#ifdef ETA_HAS_NNG
                case ObjectKind::NngSocket:
                    var_ref = alloc_compound_ref(val, dap_thread_id);
                    break;
#endif
                default: break;
            }
        }
    }
    const std::string type_hint = classify_value_type(heap, val);
    auto obj = json::object({
        {"name",               Value(name)},
        {"value",              Value(drv.format_value(val))},
        {"variablesReference", Value(static_cast<int64_t>(var_ref))},
        {"type",               Value(type_hint)},
    });
    if (indexed_count > 0) {
        obj.as_object().insert_or_assign("indexedVariables", Value(indexed_count));
    }
    if (named_count > 0) {
        obj.as_object().insert_or_assign("namedVariables", Value(named_count));
    }
    if (object_id.has_value()) {
        obj.as_object().insert_or_assign("objectId", Value(*object_id));
        obj.as_object().insert_or_assign("canInspectHeap", Value(true));
    }
    if (is_callable_type_hint(type_hint)) {
        obj.as_object().insert_or_assign("canDisassemble", Value(true));
    }
    return obj;
}

Array DapServer::expand_compound(session::Driver& drv, int dap_thread_id,
                                 uint64_t val, int start, int count) {
    using namespace runtime::nanbox;
    using namespace runtime::memory::heap;
    using namespace runtime::types;

    Array children;
    auto& heap = drv.heap();

    if (auto* cons = heap.try_get_as<ObjectKind::Cons, Cons>(
            static_cast<ObjectId>(ops::payload(val)))) {
        children.push_back(make_variable_json(drv, dap_thread_id, "car", cons->car));
        children.push_back(make_variable_json(drv, dap_thread_id, "cdr", cons->cdr));
        return children;
    }

    if (auto* vec = heap.try_get_as<ObjectKind::Vector, Vector>(
            static_cast<ObjectId>(ops::payload(val)))) {
        const std::size_t total = vec->elements.size();
        std::size_t s = (start > 0) ? static_cast<std::size_t>(start) : 0;
        if (s > total) s = total;
        std::size_t end;
        if (count > 0) {
            end = s + static_cast<std::size_t>(count);
            if (end > total) end = total;
        } else if (start > 0) {
            end = total;
        } else {
            end = (std::min)(total, std::size_t{200});
        }

        children.reserve(end - s);
        for (std::size_t i = s; i < end; ++i) {
            children.push_back(make_variable_json(drv, dap_thread_id,
                "[" + std::to_string(i) + "]", vec->elements[i]));
        }
        if (start <= 0 && count <= 0 && total > 200) {
            children.push_back(json::object({
                {"name",  "..."},
                {"value", Value("(" + std::to_string(total - 200) + " more)")},
                {"variablesReference", 0},
            }));
        }
        return children;
    }

    if (auto* cl = heap.try_get_as<ObjectKind::Closure, Closure>(
            static_cast<ObjectId>(ops::payload(val)))) {
        if (cl->func) {
            children.push_back(json::object({
                {"name",  "function"},
                {"value", Value(cl->func->name.empty() ? "<lambda>" : cl->func->name)},
                {"variablesReference", 0},
            }));
        }
        for (std::size_t i = 0; i < cl->upvals.size(); ++i) {
            std::string uname;
            if (cl->func && i < cl->func->upval_names.size() && !cl->func->upval_names[i].empty()) {
                uname = cl->func->upval_names[i];
            } else {
                uname = "&" + std::to_string(i);
            }
            children.push_back(make_variable_json(drv, dap_thread_id, uname, cl->upvals[i]));
        }
        return children;
    }

#ifdef ETA_HAS_NNG
    if (auto* ns = heap.try_get_as<ObjectKind::NngSocket, eta::nng::NngSocketPtr>(
            static_cast<ObjectId>(ops::payload(val)))) {
        children.push_back(json::object({
            {"name",  "protocol"},
            {"value", Value(std::string(eta::nng::protocol_name(ns->protocol)))},
            {"variablesReference", 0},
        }));
        children.push_back(json::object({
            {"name",  "listening"},
            {"value", Value(ns->listening ? std::string("true") : std::string("false"))},
            {"variablesReference", 0},
        }));
        children.push_back(json::object({
            {"name",  "dialed"},
            {"value", Value(ns->dialed ? std::string("true") : std::string("false"))},
            {"variablesReference", 0},
        }));
        children.push_back(json::object({
            {"name",  "closed"},
            {"value", Value(ns->closed ? std::string("true") : std::string("false"))},
            {"variablesReference", 0},
        }));
        children.push_back(json::object({
            {"name",  "pending-msgs"},
            {"value", Value(std::to_string(ns->pending_msgs.size()))},
            {"variablesReference", 0},
        }));
        return children;
    }
#endif

    return children;
}

Value DapServer::source_json_for(session::Driver& drv, uint32_t file_id) {
    if (file_id == 0) return Value{};
    if (auto* path = drv.path_for_file_id(file_id)) {
        return json::object({
            {"name", path->filename().string()},
            {"path", path->string()},
        });
    }
    return Value{};
}

} ///< namespace eta::dap

