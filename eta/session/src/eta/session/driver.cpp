#include "eta/session/driver.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "eta/interpreter/all_primitives.h"
#include "eta/nng/session_actor_runtime.h"
#include "eta/runtime/builtin_catalog.h"
#include "eta/runtime/factory.h"
#include "eta/session/repl_input.h"
#include "eta/session/runtime_config.h"
#include "eta/util/path.h"

namespace eta::session {

using eta::interpreter::register_all_primitives;

std::size_t Driver::parse_heap_env_var(const char* env_var,
                                       std::size_t default_val) noexcept {
    return ::eta::session::parse_heap_env_var(env_var, default_val);
}

Driver::Driver(ModulePathResolver resolver,
               std::size_t heap_bytes,
               std::string etai_path,
               std::vector<std::string> command_line_arguments)
    : resolver_(std::move(resolver)),
      heap_(heap_bytes),
      intern_table_(),
      registry_(),
      builtins_(),
      extensions_(),
      primitive_installer_(heap_, builtins_, extensions_),
      sidecar_manager_(*this),
      vm_(heap_, intern_table_),
      diag_engine_(),
      actor_runtime_(eta::nng::make_session_actor_runtime()),
      command_line_arguments_(std::move(command_line_arguments)),
      compilation_(),
      etac_loader_(*this, compilation_, *this),
      eval_engine_(*this, compilation_, *this, vm_),
      display_classifier_(heap_, intern_table_),
      repl_controller_(*this, display_classifier_) {
    /**
     * The VM installs a default heap GC callback in its constructor, but at
     * the Driver level we also need compiled bytecode constants in the
     * function registry to act as GC roots. Large modules can emit quoted
     * heap-backed constants long before they are executed or serialized.
     */
    heap_.set_gc_callback([this]() { collect_garbage_with_registry_roots(); });

    /**
     * Register all core primitives and native-sidecar placeholders.
     * Step 1: Populate all slots with metadata (name/arity/has_rest) + null funcs.
     */
    runtime::register_builtin_specs(builtins_);
    builtins_.begin_patching();
    register_all_primitives(
        builtins_,
        heap_,
        intern_table_,
        vm_,
        command_line_arguments_);

    /// Detect etai binary path if not explicitly supplied
    if (etai_path.empty()) {
        etai_path = detect_etai_path();
    }
    etai_path_ = std::move(etai_path);

    /**
     * Build module search path to forward to child processes.
     * Child receives this via ETA_MODULE_PATH only if ETA_MODULE_PATH is not
     * already set in the environment.
     */
    std::string module_search_path;
    {
#ifdef _WIN32
        constexpr char path_sep = ';';
#else
        constexpr char path_sep = ':';
#endif
        for (const auto& d : resolver_.dirs()) {
            if (!module_search_path.empty()) {
                module_search_path += path_sep;
            }
            module_search_path += d.string();
        }
    }
    module_search_path_ = std::move(module_search_path);

    sidecar_runtime_binding_.heap = &heap_;
    sidecar_runtime_binding_.intern_table = &intern_table_;
    sidecar_runtime_binding_.vm = &vm_;
    sidecar_runtime_binding_.function_registry = &registry_;
    sidecar_runtime_binding_.vm_globals = &vm_.globals();
    sidecar_runtime_binding_.mailbox_value =
        actor_runtime_ ? actor_runtime_->mailbox_slot() : nullptr;
    sidecar_runtime_binding_.etai_path = &etai_path_;
    sidecar_runtime_binding_.module_search_path = &module_search_path_;
    sidecar_runtime_binding_.actor_process_manager =
        actor_runtime_ ? actor_runtime_->process_manager() : nullptr;
    sidecar_runtime_binding_.process_manager =
        (sidecar_runtime_binding_.actor_process_manager != nullptr)
        ? sidecar_runtime_binding_.actor_process_manager->native_handle()
        : nullptr;
    sidecar_manager_.set_runtime_context(&sidecar_runtime_binding_);

    /// Verify every pre-registered slot now has a real implementation.
    builtins_.verify_all_patched();

    eval_engine_.install_builtin(builtins_);

    if (actor_runtime_) {
        actor_runtime_->install_worker_factories(module_search_path_);
    }

    /// Wire up function resolver.
    vm_.set_function_resolver([this](uint32_t idx) {
        return registry_.get(idx);
    });
}

Driver::~Driver() {
    const auto log_shutdown_idx = builtins_.lookup("%log-shutdown!");
    if (!log_shutdown_idx.has_value()) {
        return;
    }

    const auto& log_shutdown = builtins_.specs()[*log_shutdown_idx].func;
    if (!log_shutdown) {
        return;
    }
    (void)log_shutdown({});
}

Driver::PreludeResult Driver::load_prelude() {
    PreludeResult result;
    if (!ensure_package_sidecars_loaded(std::nullopt)) {
        return result;
    }
    return result;
}

bool Driver::has_module(const std::string& name) const {
    return compilation_.has_module(name);
}

bool Driver::clear_module_cache(const std::string& module_name) {
    const bool changed = compilation_.clear_module_cache(module_name);
    if (changed) {
        repl_controller_.forget_module(module_name);
    }
    return changed;
}

bool Driver::run_file(const fs::path& path) {
    if (!ensure_package_sidecars_loaded(path.parent_path())) {
        return false;
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        diag_engine_.emit_error(
            diagnostic::DiagnosticCode::ModuleNotFound, {},
            "cannot open file: " + path.string());
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    const auto file_id = allocate_file_id(path.string());
    return compilation_.run_source_impl(*this, buf.str(), file_id);
}

std::optional<Driver::CompileResult> Driver::compile_file(const fs::path& path) {
    if (!ensure_package_sidecars_loaded(path.parent_path())) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        diag_engine_.emit_error(
            diagnostic::DiagnosticCode::ModuleNotFound, {},
            "cannot open file: " + path.string());
        return std::nullopt;
    }
    std::ostringstream buf;
    buf << in.rdbuf();

    const auto file_id = allocate_file_id(path.string());
    CompileResult cr;
    if (!compilation_.run_source_impl(
            *this,
            buf.str(),
            file_id,
            /*result=*/nullptr,
            /*result_binding=*/{},
            /*execute=*/false,
            &cr)) {
        return std::nullopt;
    }
    return cr;
}

Driver::PreludeResult Driver::compile_prelude() {
    return load_prelude();
}

bool Driver::run_source(std::string_view source,
                        runtime::nanbox::LispVal* result,
                        const std::string& result_binding) {
    if (!ensure_package_sidecars_loaded(std::nullopt)) {
        return false;
    }
    return compilation_.run_source_impl(
        *this,
        std::string(source),
        /*file_id=*/0,
        result,
        result_binding);
}

bool Driver::eval_string(std::string source, std::string& out) {
    return repl_controller_.eval_string(std::move(source), out);
}

Driver::CompletionResult Driver::completions_at(
    const std::string& source,
    std::size_t cursor_pos) const {
    return repl_controller_.completions_at(source, cursor_pos);
}

std::string Driver::hover_at(const std::string& symbol) const {
    return repl_controller_.hover_at(symbol);
}

bool Driver::is_complete_expression(const std::string& src,
                                    std::string* indent_hint) const {
    return is_complete_repl_input(src, indent_hint);
}

void Driver::request_interrupt() {
    vm_.request_interrupt();
}

DisplayValue Driver::eval_to_display(const std::string& source) {
    return repl_controller_.eval_to_display(source);
}

void Driver::set_stream_sinks(StreamSink stdout_sink, StreamSink stderr_sink) {
    auto stdout_for_children = stdout_sink;
    auto stderr_for_children = stderr_sink;

    if (stdout_sink) {
        set_output_port(std::make_shared<runtime::CallbackPort>(
            [sink = std::move(stdout_sink)](const std::string& text) {
                sink(text);
            }));
    }
    if (stderr_sink) {
        set_error_port(std::make_shared<runtime::CallbackPort>(
            [sink = std::move(stderr_sink)](const std::string& text) {
                sink(text);
            }));
    }

    /**
     * Capture the active sink routing in spawn-thread factories so child
     * actor output keeps publishing to the same notebook stream.
     */
    if (actor_runtime_) {
        actor_runtime_->install_worker_factories(
            module_search_path_,
            std::move(stdout_for_children),
            std::move(stderr_for_children));
    }
}

void Driver::on_actor_lifecycle(
    std::function<void(const ActorEvent&)> on_event) {
    if (actor_runtime_) {
        actor_runtime_->on_actor_lifecycle(std::move(on_event));
    }
}

diagnostic::DiagnosticEngine& Driver::diagnostics() noexcept {
    return diag_engine_;
}

const diagnostic::DiagnosticEngine& Driver::diagnostics() const noexcept {
    return diag_engine_;
}

diagnostic::FileResolver Driver::file_resolver() const {
    return source_files_.file_resolver();
}

ModulePathResolver& Driver::resolver() noexcept {
    return resolver_;
}

runtime::vm::VM& Driver::vm() noexcept {
    return vm_;
}

const runtime::vm::VM& Driver::vm() const noexcept {
    return vm_;
}

semantics::BytecodeFunctionRegistry& Driver::registry() noexcept {
    return registry_;
}

const semantics::BytecodeFunctionRegistry& Driver::registry() const noexcept {
    return registry_;
}

const fs::path* Driver::path_for_file_id(uint32_t id) const noexcept {
    return source_files_.path_for_file_id(id);
}

void Driver::set_output_port(std::shared_ptr<runtime::Port> port) {
    auto val = runtime::memory::factory::make_port(heap_, std::move(port));
    if (val) {
        vm_.set_current_output_port(*val);
    }
}

void Driver::set_error_port(std::shared_ptr<runtime::Port> port) {
    auto val = runtime::memory::factory::make_port(heap_, std::move(port));
    if (val) {
        vm_.set_current_error_port(*val);
    }
}

uint32_t Driver::ensure_file_id(const fs::path& path) {
    return source_files_.ensure_file_id(path);
}

uint32_t Driver::file_id_for_path(const std::string& path) const {
    return source_files_.file_id_for_path(path);
}

std::set<uint32_t> Driver::valid_lines_for(uint32_t file_id) const {
    return source_files_.valid_lines_for(file_id, registry_);
}

std::string Driver::format_value(runtime::nanbox::LispVal v,
                                 runtime::FormatMode mode) {
    return runtime::format_value(v, mode, heap_, intern_table_);
}

bool Driver::install_mailbox(const std::string& endpoint) {
    if (!actor_runtime_) {
        return false;
    }
    return actor_runtime_->install_mailbox(heap_, endpoint);
}

native::ActorProcessManager* Driver::process_manager() noexcept {
    if (!actor_runtime_) {
        return nullptr;
    }
    return actor_runtime_->process_manager();
}

const native::ActorProcessManager* Driver::process_manager() const noexcept {
    if (!actor_runtime_) {
        return nullptr;
    }
    return actor_runtime_->process_manager();
}

runtime::nanbox::LispVal Driver::mailbox() const noexcept {
    if (!actor_runtime_) {
        return runtime::nanbox::Nil;
    }
    return actor_runtime_->mailbox();
}

const std::unordered_map<uint32_t, std::string>& Driver::global_names() const noexcept {
    return compilation_.global_names();
}

runtime::memory::heap::Heap& Driver::heap() noexcept {
    return heap_;
}

const runtime::memory::heap::Heap& Driver::heap() const noexcept {
    return heap_;
}

runtime::memory::intern::InternTable& Driver::intern_table() noexcept {
    return intern_table_;
}

semantics::OptimizationPipeline& Driver::optimization_pipeline() noexcept {
    return optimization_pipeline_;
}

std::size_t Driver::builtin_count() const noexcept {
    return primitive_installer_.builtin_count();
}

std::uint64_t Driver::extension_env_hash() const noexcept {
    return extensions_.fingerprint();
}

void Driver::register_extension_primitive(std::string name,
                                          uint32_t arity,
                                          bool has_rest,
                                          runtime::types::PrimitiveFunc func) {
    if (!compilation_.can_register_extension_primitives()) {
        std::cerr
            << "register_extension_primitive must be called before module execution\n";
        std::abort();
    }
    extensions_.register_extension(
        std::move(name), arity, has_rest, std::move(func));
    primitive_installer_.invalidate();
}

std::size_t Driver::extension_primitive_count() const noexcept {
    return extensions_.size();
}

bool Driver::load_package_sidecars(const fs::path& start_dir) {
    return ensure_package_sidecars_loaded(start_dir);
}

bool Driver::run_etac_file(const fs::path& path) {
    return etac_loader_.run_etac_file(path);
}

bool Driver::can_register_extension_primitives() const noexcept {
    return compilation_.can_register_extension_primitives();
}

void Driver::register_builtin_primitive(std::string name,
                                        uint32_t arity,
                                        bool has_rest,
                                        runtime::types::PrimitiveFunc func) {
    builtins_.register_builtin(
        std::move(name), arity, has_rest, std::move(func));
}

bool Driver::has_builtin_primitive(std::string_view name) const {
    return builtins_.lookup(name).has_value();
}

void Driver::overwrite_builtin_primitive(std::string_view name,
                                         runtime::types::PrimitiveFunc func) {
    builtins_.overwrite_func(name, std::move(func));
}

void Driver::invalidate_primitive_installer() {
    primitive_installer_.invalidate();
}

void Driver::emit_sidecar_error(std::string message) {
    diag_engine_.emit_error(
        diagnostic::DiagnosticCode::ModuleNotFound, {},
        std::move(message));
}

bool Driver::ensure_package_sidecars_loaded(std::optional<fs::path> start_dir) {
    return sidecar_manager_.ensure_package_sidecars_loaded(
        std::move(start_dir), resolver_.dirs(), etai_path_);
}

std::size_t Driver::total_primitive_count() const noexcept {
    return primitive_installer_.total_primitive_count();
}

runtime::BuiltinEnvironment& Driver::builtins() noexcept {
    return builtins_;
}

runtime::ExtensionEnvironment& Driver::extensions() noexcept {
    return extensions_;
}

RuntimePrimitiveInstaller& Driver::primitive_installer() noexcept {
    return primitive_installer_;
}

bool Driver::run_module_file(const fs::path& path) {
    if (path.extension() == ".etac") {
        return run_etac_file(path);
    }
    return run_file(path);
}

bool Driver::run_source_file(const fs::path& path) {
    return run_file(path);
}

std::optional<fs::path> Driver::resolve_import_path(const std::string& module_name,
                                                    bool* shadow_conflict) {
    auto candidates = resolver_.resolve_all(module_name);
    if (candidates.empty()) {
        if (shadow_conflict) {
            *shadow_conflict = false;
        }
        return std::nullopt;
    }

    if (resolver_.strict_shadow_scan() && candidates.size() > 1u) {
        if (shadow_conflict) {
            *shadow_conflict = true;
        }
        std::ostringstream oss;
        oss << "strict shadow mode: module '" << module_name
            << "' resolves to multiple files";
        for (const auto& candidate : candidates) {
            oss << "\n  - " << candidate.string();
        }
        diag_engine_.emit_error(
            diagnostic::DiagnosticCode::ModuleNotFound, {},
            oss.str());
        return std::nullopt;
    }

    if (shadow_conflict) {
        *shadow_conflict = false;
    }
    return candidates.front();
}

std::string Driver::diagnostics_to_string() const {
    std::ostringstream oss;
    diag_engine_.print_all(oss, /*use_color=*/false, file_resolver());
    return oss.str();
}

std::vector<std::string> Driver::discover_module_names() const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;

    for (const auto& root : resolver_.dirs()) {
        std::error_code ec;
        if (!fs::is_directory(root, ec) || ec) {
            continue;
        }

        fs::recursive_directory_iterator it(
            root,
            fs::directory_options::skip_permission_denied,
            ec);
        fs::recursive_directory_iterator end;
        if (ec) {
            continue;
        }

        while (it != end) {
            const auto entry = *it;
            it.increment(ec);
            if (ec) {
                ec.clear();
                continue;
            }

            if (!entry.is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }

            const auto path = entry.path();
            if (path.extension() != ".eta") {
                continue;
            }

            auto rel = fs::relative(path, root, ec);
            if (ec) {
                ec.clear();
                continue;
            }

            auto mod = rel.generic_string();
            if (!mod.ends_with(".eta")) {
                continue;
            }
            mod.resize(mod.size() - 4);
            std::replace(mod.begin(), mod.end(), '/', '.');

            if (!mod.empty() && seen.insert(mod).second) {
                out.push_back(std::move(mod));
            }
        }
    }

    return out;
}

void Driver::collect_garbage_with_registry_roots() {
    auto roots = heap_.make_external_root_frame();
    for (const auto& func : registry_.all()) {
        for (auto c : func.constants) {
            if (runtime::nanbox::ops::is_boxed(c) &&
                runtime::nanbox::ops::tag(c) == runtime::nanbox::Tag::HeapObject) {
                roots.push(c);
            }
        }
    }
    vm_.collect_garbage();
}

std::string Driver::detect_etai_path() {
    const fs::path fallback = util::sibling_executable_path("etai").filename();
    const auto candidate = util::sibling_executable_path("etai");
    if (!candidate.is_absolute()) {
        return fallback.string();
    }

    std::error_code ec;
    if (fs::is_regular_file(candidate, ec) && !ec) {
        return candidate.string();
    }
    return fallback.string();
}

uint32_t Driver::allocate_file_id(const std::string& raw_path) {
    return source_files_.allocate_file_id(raw_path);
}

bool Driver::hydrate_executed_module_source(const std::string& module_name) {
    return compilation_.hydrate_executed_module_source(*this, module_name);
}

void Driver::emit_link_error(const reader::LinkError& e) {
    diag_engine_.emit(diagnostic::to_diagnostic(e));
}

void Driver::emit_runtime_error(const runtime::error::RuntimeError& err) {
    std::visit([this](auto&& e) {
        diag_engine_.emit(diagnostic::to_diagnostic(e));
    }, err);
}

} // namespace eta::session
