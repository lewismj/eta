/// VM debugger introspection: frames, locals and upvalues.
#include "vm.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <limits>
#include "eta/runtime/actor/actor_system.h"
#include "eta/runtime/builtin_catalog.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/ad_error.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/memory/value_visit.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/port.h"
#include "eta/runtime/clp/domain.h"
#include "eta/runtime/prof/profiler.h"
#include <bit>

namespace eta::runtime::vm {

using namespace eta::runtime::memory::value_visit;

using namespace eta::runtime::memory::factory;
using namespace eta::runtime::types;
namespace prof = eta::runtime::prof;

/**
 * Debug / inspect API
 */


std::vector<FrameInfo> VM::get_frames() const {
    std::vector<FrameInfo> result;

    /// Current (innermost) frame
    if (current_func_) {
        FrameInfo fi;
        fi.func_name  = current_func_->name;
        /**
         * When the VM is paused at the debug hook, pc_ points to the
         * about-to-execute instruction (pre-increment).  Using pc_ - 1
         * Use the exact span saved by DebugState when it stopped.
         */
        if (debug_ && debug_->is_paused()) {
            fi.span = debug_->stopped_span();
        } else {
            fi.span = current_func_->span_at(pc_ > 0 ? pc_ - 1 : 0);
        }
        fi.frame_index = 0;
        result.push_back(fi);
    }

    /// Walk saved frames in reverse order (most recent caller first)
    for (int i = static_cast<int>(frames_.size()) - 1; i >= 0; --i) {
        const auto& f = frames_[static_cast<std::size_t>(i)];
        if (f.kind == FrameKind::Sentinel) break; ///< stop at execution boundary
        if (f.func == nullptr) continue;
        FrameInfo fi;
        fi.func_name  = f.func->name;
        /// pc in a saved frame points to the instruction *after* the call site
        uint32_t call_pc = f.pc > 0 ? f.pc - 1 : 0;
        fi.span        = f.func->span_at(call_pc);
        fi.frame_index = result.size();
        result.push_back(fi);
    }
    return result;
}

std::vector<VarEntry> VM::get_locals(std::size_t frame_index) const {
    std::vector<VarEntry> result;

    const BytecodeFunction* func = nullptr;
    uint32_t frame_fp = fp_;

    if (frame_index == 0) {
        func     = current_func_;
        frame_fp = fp_;
    } else {
        /// Walk saved frames (most recent caller first)
        std::size_t idx = 1;
        for (int i = static_cast<int>(frames_.size()) - 1; i >= 0; --i) {
            const auto& f = frames_[static_cast<std::size_t>(i)];
            if (f.kind == FrameKind::Sentinel) break;
            if (f.func == nullptr) continue;
            if (idx == frame_index) {
                func     = f.func;
                frame_fp = f.fp;
                break;
            }
            ++idx;
        }
    }

    if (!func) return result;

    uint32_t num_params = func->arity + (func->has_rest ? 1 : 0);
    /**
     * Only expose the slots that the emitter gave real names to.
     * stack_size includes +32 temporary headroom that we must not expose as
     * populated (e.g. module-init functions) fall back to num_params or, if
     * that is also zero, scan the stack for meaningful (non-Nil) values so the
     * Variables panel is not completely empty.
     */
    uint32_t num_slots;
    if (!func->local_names.empty()) {
        num_slots = static_cast<uint32_t>(func->local_names.size());
    } else if (num_params > 0) {
        num_slots = num_params;
    } else {
        /// Module-init: expose occupied stack slots up to stack_size minus headroom
        uint32_t headroom = 32;
        num_slots = func->stack_size > headroom ? func->stack_size - headroom : func->stack_size;
        /// Clamp to actual stack extent
        uint32_t avail = static_cast<uint32_t>(stack_.size()) - frame_fp;
        if (num_slots > avail) num_slots = avail;
    }
    for (uint32_t slot = 0; slot < num_slots; ++slot) {
        std::size_t stack_idx = static_cast<std::size_t>(frame_fp) + slot;
        if (stack_idx >= stack_.size()) break;
        VarEntry e;
        /// Use the real name from local_names if available; fall back to %N placeholder.
        if (slot < func->local_names.size() && !func->local_names[slot].empty()) {
            e.name = func->local_names[slot];
        } else {
            e.name = "%" + std::to_string(slot);
        }
        e.value    = stack_[stack_idx];
        e.is_param = (slot < num_params);
        result.push_back(e);
    }
    return result;
}

bool VM::set_local(std::size_t frame_index, std::size_t slot, LispVal value) {
    const BytecodeFunction* func = nullptr;
    uint32_t frame_fp = fp_;

    if (frame_index == 0) {
        func = current_func_;
        frame_fp = fp_;
    } else {
        std::size_t idx = 1;
        for (int i = static_cast<int>(frames_.size()) - 1; i >= 0; --i) {
            const auto& f = frames_[static_cast<std::size_t>(i)];
            if (f.kind == FrameKind::Sentinel) break;
            if (f.func == nullptr) continue;
            if (idx == frame_index) {
                func = f.func;
                frame_fp = f.fp;
                break;
            }
            ++idx;
        }
    }

    if (!func) return false;

    uint32_t num_params = func->arity + (func->has_rest ? 1 : 0);
    uint32_t num_slots;
    if (!func->local_names.empty()) {
        num_slots = static_cast<uint32_t>(func->local_names.size());
    } else if (num_params > 0) {
        num_slots = num_params;
    } else {
        uint32_t headroom = 32;
        num_slots = func->stack_size > headroom ? func->stack_size - headroom : func->stack_size;
        uint32_t avail = static_cast<uint32_t>(stack_.size()) - frame_fp;
        if (num_slots > avail) num_slots = avail;
    }

    if (slot >= num_slots) return false;

    const std::size_t stack_idx = static_cast<std::size_t>(frame_fp) + slot;
    if (stack_idx >= stack_.size()) return false;

    stack_[stack_idx] = value;
    return true;
}

std::optional<nanbox::LispVal> VM::get_frame_closure(std::size_t frame_index) const {
    LispVal closure_val = current_closure_;

    if (frame_index != 0) {
        std::size_t idx = 1;
        bool found = false;
        for (int i = static_cast<int>(frames_.size()) - 1; i >= 0; --i) {
            const auto& f = frames_[static_cast<std::size_t>(i)];
            if (f.kind == FrameKind::Sentinel) break;
            if (f.func == nullptr) continue;
            if (idx == frame_index) {
                closure_val = f.closure;
                found = true;
                break;
            }
            ++idx;
        }
        if (!found) {
            return std::nullopt;
        }
    }

    if (try_get_as<ObjectKind::Closure, types::Closure>(closure_val)) {
        return closure_val;
    }
    return std::nullopt;
}

std::vector<VarEntry> VM::get_upvalues(std::size_t frame_index) const {
    std::vector<VarEntry> result;

    const auto closure_opt = get_frame_closure(frame_index);
    if (!closure_opt.has_value()) {
        return result;
    }

    if (auto* cl = try_get_as<ObjectKind::Closure, types::Closure>(*closure_opt)) {
        for (std::size_t i = 0; i < cl->upvals.size(); ++i) {
            VarEntry e;
            /// Use the real name from upval_names if available; fall back to &N placeholder.
            if (cl->func && i < cl->func->upval_names.size() && !cl->func->upval_names[i].empty()) {
                e.name = cl->func->upval_names[i];
            } else {
                e.name = "&" + std::to_string(i);
            }
            e.value = cl->upvals[i];
            result.push_back(e);
        }
    }
    return result;
}
} ///< namespace eta::runtime::vm

