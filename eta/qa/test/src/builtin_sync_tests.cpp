#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/builtin_catalog.h>
#include <eta/runtime/builtin_env.h>
#include <eta/runtime/builtin_metadata.h>
#include <eta/runtime/core_primitives.h>
#include <eta/reader/special_form_docs.h>
#include <eta/interpreter/all_primitives.h>
#include <eta/runtime/os_primitives.h>
#include <eta/runtime/process_primitives.h>
#include <eta/runtime/time_primitives.h>
#include <eta/runtime/vm/vm.h>
#include <eta/torch/torch_primitives.h>
#include <eta/stats/stats_primitives.h>
#include <eta/log/log_primitives.h>

using namespace eta::runtime;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;

BOOST_AUTO_TEST_SUITE(builtin_sync_tests)

/**
 * Verify that register_builtin_specs() contains entries for
 * every builtin that the runtime modules register.
 *
 * We check os, process, time, torch, stats, and log individually (os/log require a live VM;
 * the others accept a null VM
 * pointer).
 * Port/IO/NNG require a live VM or driver-specific args, so full end-to-end
 * coverage is provided by the Driver constructor's verify_all_patched() call.
 */
BOOST_AUTO_TEST_CASE(names_ssot_contains_os_process_time_torch_stats_and_log) {
    /// 1. Analysis registration metadata from the builtin catalog
    BuiltinEnvironment names_env;
    register_builtin_specs(names_env);

    /// 2. OS primitives
    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment os_env;
    register_os_primitives(os_env, heap, intern, vm);

    for (size_t i = 0; i < os_env.size(); ++i) {
        auto idx = names_env.lookup(os_env.specs()[i].name);
        BOOST_TEST_CONTEXT("os builtin: " << os_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == os_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == os_env.specs()[i].has_rest);
        }
    }

    /// 3. Process primitives
    BuiltinEnvironment process_env;
    register_process_primitives(process_env, heap, intern, vm);

    for (size_t i = 0; i < process_env.size(); ++i) {
        auto idx = names_env.lookup(process_env.specs()[i].name);
        BOOST_TEST_CONTEXT("process builtin: " << process_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == process_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == process_env.specs()[i].has_rest);
        }
    }

    /// 4. Time primitives
    BuiltinEnvironment time_env;
    register_time_primitives(time_env, heap, intern, nullptr);

    for (size_t i = 0; i < time_env.size(); ++i) {
        auto idx = names_env.lookup(time_env.specs()[i].name);
        BOOST_TEST_CONTEXT("time builtin: " << time_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == time_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == time_env.specs()[i].has_rest);
        }
    }

    /// 5. Torch primitives
    BuiltinEnvironment torch_env;
    eta::torch_bindings::register_torch_primitives(torch_env, heap, intern, nullptr);

    /// Every torch name must appear in the SSoT with matching metadata
    for (size_t i = 0; i < torch_env.size(); ++i) {
        auto idx = names_env.lookup(torch_env.specs()[i].name);
        BOOST_TEST_CONTEXT("torch builtin: " << torch_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == torch_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == torch_env.specs()[i].has_rest);
        }
    }

    /// 6. Stats primitives
    BuiltinEnvironment stats_env;
    eta::stats_bindings::register_stats_primitives(stats_env, heap, intern, nullptr);

    for (size_t i = 0; i < stats_env.size(); ++i) {
        auto idx = names_env.lookup(stats_env.specs()[i].name);
        BOOST_TEST_CONTEXT("stats builtin: " << stats_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == stats_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == stats_env.specs()[i].has_rest);
        }
    }

    /// 7. Log primitives
    BuiltinEnvironment log_env;
    eta::log::register_log_primitives(log_env, heap, intern, &vm);

    for (size_t i = 0; i < log_env.size(); ++i) {
        auto idx = names_env.lookup(log_env.specs()[i].name);
        BOOST_TEST_CONTEXT("log builtin: " << log_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == log_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == log_env.specs()[i].has_rest);
        }
    }
}

/**
 * Verify basic properties of analysis builtin registration:
 * non-empty, no duplicate names.
 */
BOOST_AUTO_TEST_CASE(names_ssot_no_duplicates) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    BOOST_TEST(env.size() > 0u);

    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;
    for (size_t i = 0; i < env.size(); ++i) {
        if (!seen.insert(env.specs()[i].name).second) {
            duplicates.push_back(env.specs()[i].name);
        }
    }
    if (!duplicates.empty()) {
        std::string msg = "Duplicate builtin names:";
        for (const auto& d : duplicates) msg += " " + d;
        BOOST_FAIL(msg);
    }
}

BOOST_AUTO_TEST_CASE(catalog_has_no_duplicate_names) {
    const auto catalog = builtin_catalog();
    BOOST_TEST(catalog.size() > 0u);

    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;
    for (const auto& entry : catalog) {
        if (!seen.insert(entry.name).second) {
            duplicates.push_back(entry.name);
        }
    }

    if (!duplicates.empty()) {
        std::string msg = "Duplicate builtin catalog names:";
        for (const auto& d : duplicates) msg += " " + d;
        BOOST_FAIL(msg);
    }
}

BOOST_AUTO_TEST_CASE(catalog_matches_analysis_registration_metadata) {
    BuiltinEnvironment specs_env;
    register_builtin_specs(specs_env);

    const auto catalog = builtin_catalog();
    BOOST_REQUIRE_EQUAL(catalog.size(), specs_env.size());

    constexpr std::array<std::string_view, 5> allowed_owners = {
        "core",
        "sidecar:eta-torch",
        "sidecar:eta-stats",
        "sidecar:eta-log",
        "sidecar:eta-nng"
    };

    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& entry = catalog[i];
        const auto& spec = specs_env.specs()[i];

        const bool owner_allowed = std::find(
            allowed_owners.begin(),
            allowed_owners.end(),
            std::string_view(entry.owner))
            != allowed_owners.end();

        BOOST_TEST_CONTEXT("slot " << i << " name=" << entry.name) {
            BOOST_TEST(!entry.name.empty());
            BOOST_TEST(owner_allowed);
            BOOST_TEST(entry.name == spec.name);
            BOOST_TEST(entry.arity == spec.arity);
            BOOST_TEST(entry.has_rest == spec.has_rest);
        }
    }
}

BOOST_AUTO_TEST_CASE(catalog_registration_adapter_matches_catalog_exactly) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    const auto catalog = builtin_catalog();
    BOOST_REQUIRE_EQUAL(env.size(), catalog.size());

    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& spec = env.specs()[i];
        const auto& entry = catalog[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << entry.name) {
            BOOST_TEST(spec.name == entry.name);
            BOOST_TEST(spec.arity == entry.arity);
            BOOST_TEST(spec.has_rest == entry.has_rest);
            BOOST_TEST(!spec.func);
        }
    }
}

BOOST_AUTO_TEST_CASE(catalog_order_matches_runtime_registration_order) {
    const auto catalog = builtin_catalog();

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment runtime_env;
    eta::interpreter::register_all_primitives(runtime_env, heap, intern, vm);

    BOOST_REQUIRE_EQUAL(runtime_env.size(), catalog.size());
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& spec = runtime_env.specs()[i];
        const auto& entry = catalog[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << entry.name) {
            BOOST_TEST(spec.name == entry.name);
            BOOST_TEST(spec.arity == entry.arity);
            BOOST_TEST(spec.has_rest == entry.has_rest);
            BOOST_TEST(static_cast<bool>(spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_registration_matches_metadata_prefix_exactly) {
    BuiltinEnvironment specs_env;
    register_builtin_specs(specs_env);
    const auto eval_slot = specs_env.lookup("eval");
    BOOST_REQUIRE(eval_slot.has_value());
    const auto expected_core_size = *eval_slot + 1;

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    BOOST_TEST(core_env.size() > 0u);
    BOOST_REQUIRE_EQUAL(core_env.size(), expected_core_size);
    BOOST_REQUIRE(core_env.size() <= specs_env.size());

    for (std::size_t i = 0; i < core_env.size(); ++i) {
        const auto& runtime_spec = core_env.specs()[i];
        const auto& seeded_spec = specs_env.specs()[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << runtime_spec.name) {
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_arithmetic_and_predicate_window_matches_seeded_slots) {
    constexpr std::array<std::string_view, 33> expected_window = {{
        "+", "-", "*", "/", "=", "<", ">", "<=", ">=", "eq?", "eqv?", "not",
        "cons", "car", "cdr", "pair?", "null?", "list",
        "number?", "boolean?", "string?", "char?", "symbol?", "procedure?",
        "integer?", "zero?", "positive?", "negative?",
        "abs", "min", "max", "modulo", "remainder"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    BOOST_REQUIRE(core_env.size() >= expected_window.size());
    BOOST_REQUIRE(seeded_env.size() >= expected_window.size());

    for (std::size_t i = 0; i < expected_window.size(); ++i) {
        const auto& runtime_spec = core_env.specs()[i];
        const auto& seeded_spec = seeded_env.specs()[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << expected_window[i]) {
            BOOST_TEST(runtime_spec.name == expected_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_math_and_aad_policy_window_matches_seeded_slots) {
    constexpr std::size_t kMathWindowStart = 33u;
    constexpr std::array<std::string_view, 12> expected_window = {{
        "sin", "cos", "tan", "asin", "acos", "atan", "exp", "log", "sqrt",
        "pow", "set-aad-nondiff-policy!", "aad-nondiff-policy"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    BOOST_REQUIRE(core_env.size() >= kMathWindowStart + expected_window.size());
    BOOST_REQUIRE(seeded_env.size() >= kMathWindowStart + expected_window.size());

    for (std::size_t i = 0; i < expected_window.size(); ++i) {
        const auto slot = kMathWindowStart + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << expected_window[i]) {
            BOOST_TEST(runtime_spec.name == expected_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_sequences_and_collections_windows_match_seeded_slots) {
    constexpr std::array<std::string_view, 16> sequence_window = {{
        "length", "append", "reverse", "list-ref", "list-tail", "set-car!",
        "set-cdr!", "assq", "assoc", "member", "symbol->string",
        "string->symbol", "apply", "map", "for-each", "equal?"
    }};

    constexpr std::array<std::string_view, 36> collections_window = {{
        "vector", "vector-length", "vector-ref", "vector-set!", "vector?",
        "make-vector", "hash-map", "make-hash-map", "hash-map?",
        "hash-map-ref", "hash-map-assoc", "hash-map-dissoc", "hash-map-keys",
        "hash-map-values", "hash-map-size", "hash-map->list",
        "list->hash-map", "hash-map-fold", "hash", "make-hash-set",
        "hash-set", "hash-set?", "hash-set-add", "hash-set-remove",
        "hash-set-contains?", "hash-set-union", "hash-set-intersect",
        "hash-set-diff", "hash-set->list", "list->hash-set", "%atom-new",
        "%atom?", "%atom-deref", "%atom-reset!", "%atom-compare-and-set!",
        "%atom-swap!"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    const auto seq_start = seeded_env.lookup(std::string(sequence_window.front()));
    BOOST_REQUIRE(seq_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *seq_start + sequence_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *seq_start + sequence_window.size());

    for (std::size_t i = 0; i < sequence_window.size(); ++i) {
        const auto slot = *seq_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << sequence_window[i]) {
            BOOST_TEST(runtime_spec.name == sequence_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }

    const auto collections_start =
        seeded_env.lookup(std::string(collections_window.front()));
    BOOST_REQUIRE(collections_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *collections_start + collections_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *collections_start + collections_window.size());

    for (std::size_t i = 0; i < collections_window.size(); ++i) {
        const auto slot = *collections_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << collections_window[i]) {
            BOOST_TEST(runtime_spec.name == collections_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_strings_and_delegate_windows_match_seeded_slots) {
    constexpr std::array<std::string_view, 50> expected_window = {{
        "string-length", "string-append", "number->string", "string->number",
        "string-ref", "substring", "string=?", "string<?", "string>?",
        "string<=?", "string>=?",
        "%csv-open-reader", "%csv-reader-from-string", "%csv-columns",
        "%csv-read-row", "%csv-read-record", "%csv-read-typed-row",
        "%csv-close", "%csv-open-writer", "%csv-write-row", "%csv-write-record",
        "%csv-flush", "%fact-table-load-csv", "%fact-table-save-csv",
        "%csv-reader?", "%csv-writer?", "%regex-compile", "%regex?",
        "%regex-pattern", "%regex-flags", "%regex-match?", "%regex-search",
        "%regex-find-all", "%regex-replace", "%regex-replace-fn", "%regex-split",
        "%regex-quote", "%regex-match?-str", "%regex-search-str",
        "%regex-find-all-str", "%regex-replace-str", "%regex-split-str",
        "%regex-cache-stats", "%regex-cache-reset!", "%json-read",
        "%json-read-string", "%json-write", "%json-write-string",
        "char->integer", "integer->char"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    const auto strings_start = seeded_env.lookup(std::string(expected_window.front()));
    BOOST_REQUIRE(strings_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *strings_start + expected_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *strings_start + expected_window.size());

    for (std::size_t i = 0; i < expected_window.size(); ++i) {
        const auto slot = *strings_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << expected_window[i]) {
            BOOST_TEST(runtime_spec.name == expected_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_misc_windows_match_seeded_slots) {
    constexpr std::array<std::string_view, 9> prof_window = {{
        "error",
        "platform",
        "%prof-start",
        "%prof-stop",
        "%prof-report",
        "%prof-counter",
        "%prof-region-enter",
        "%prof-region-exit",
        "%prof-enabled?"
    }};

    constexpr std::array<std::string_view, 5> lifecycle_window = {{
        "register-finalizer!",
        "unregister-finalizer!",
        "make-guardian",
        "guardian-track!",
        "guardian-collect"
    }};

    constexpr std::array<std::string_view, 1> eval_window = {{
        "eval"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    const auto prof_start = seeded_env.lookup(std::string(prof_window.front()));
    BOOST_REQUIRE(prof_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *prof_start + prof_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *prof_start + prof_window.size());
    for (std::size_t i = 0; i < prof_window.size(); ++i) {
        const auto slot = *prof_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << prof_window[i]) {
            BOOST_TEST(runtime_spec.name == prof_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }

    const auto lifecycle_start =
        seeded_env.lookup(std::string(lifecycle_window.front()));
    BOOST_REQUIRE(lifecycle_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *lifecycle_start + lifecycle_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *lifecycle_start + lifecycle_window.size());
    for (std::size_t i = 0; i < lifecycle_window.size(); ++i) {
        const auto slot = *lifecycle_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << lifecycle_window[i]) {
            BOOST_TEST(runtime_spec.name == lifecycle_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }

    const auto eval_start = seeded_env.lookup(std::string(eval_window.front()));
    BOOST_REQUIRE(eval_start.has_value());
    BOOST_REQUIRE(core_env.size() > *eval_start);
    BOOST_REQUIRE(seeded_env.size() > *eval_start);
    {
        const auto& runtime_spec = core_env.specs()[*eval_start];
        const auto& seeded_spec = seeded_env.specs()[*eval_start];
        BOOST_TEST_CONTEXT("slot " << *eval_start << " name=" << eval_window[0]) {
            BOOST_TEST(runtime_spec.name == eval_window[0]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_logic_windows_match_seeded_slots) {
    constexpr std::array<std::string_view, 25> logic_window = {{
        "logic-var?",
        "register-finalizer!",
        "unregister-finalizer!",
        "make-guardian",
        "guardian-track!",
        "guardian-collect",
        "put-attr",
        "get-attr",
        "del-attr",
        "attr-var?",
        "register-attr-hook!",
        "logic-var/named",
        "var-name",
        "set-occurs-check!",
        "occurs-check-mode",
        "ground?",
        "compound?",
        "term",
        "functor",
        "arity",
        "arg",
        "dual?",
        "dual-primal",
        "dual-backprop",
        "make-dual"
    }};

    constexpr std::array<std::string_view, 1> logic_tail_window = {{
        "register-prop-attr!"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    const auto logic_start = seeded_env.lookup(std::string(logic_window.front()));
    BOOST_REQUIRE(logic_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *logic_start + logic_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *logic_start + logic_window.size());
    for (std::size_t i = 0; i < logic_window.size(); ++i) {
        const auto slot = *logic_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << logic_window[i]) {
            BOOST_TEST(runtime_spec.name == logic_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }

    const auto logic_tail_start =
        seeded_env.lookup(std::string(logic_tail_window.front()));
    BOOST_REQUIRE(logic_tail_start.has_value());
    BOOST_REQUIRE(core_env.size() > *logic_tail_start);
    BOOST_REQUIRE(seeded_env.size() > *logic_tail_start);
    {
        const auto& runtime_spec = core_env.specs()[*logic_tail_start];
        const auto& seeded_spec = seeded_env.specs()[*logic_tail_start];
        BOOST_TEST_CONTEXT(
            "slot " << *logic_tail_start
                    << " name=" << logic_tail_window.front()) {
            BOOST_TEST(runtime_spec.name == logic_tail_window.front());
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_clp_windows_match_seeded_slots) {
    constexpr std::array<std::string_view, 29> clp_window = {{
        "%clp-domain-z!",
        "%clp-domain-fd!",
        "%clp-domain-r!",
        "%clp-get-domain",
        "%clp-linearize",
        "%clp-fm-feasible?",
        "%clp-fm-bounds",
        "%clp-r-post-leq!",
        "%clp-r-post-eq!",
        "%clp-r-propagate!",
        "%clp-r-minimize",
        "%clp-r-maximize",
        "%clp-r-qp-minimize",
        "%clp-r-qp-maximize",
        "%clp-fd-plus!",
        "%clp-fd-plus-offset!",
        "%clp-fd-abs!",
        "%clp-fd-times!",
        "%clp-fd-sum!",
        "%clp-fd-scalar-product!",
        "%clp-fd-element!",
        "%clp-fd-all-different!",
        "%clp-bool-and!",
        "%clp-bool-or!",
        "%clp-bool-xor!",
        "%clp-bool-imp!",
        "%clp-bool-eq!",
        "%clp-bool-not!",
        "%clp-bool-card!"
    }};

    constexpr std::array<std::string_view, 1> clp_tail_window = {{
        "%clp-prop-queue-size"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    const auto clp_start = seeded_env.lookup(std::string(clp_window.front()));
    BOOST_REQUIRE(clp_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *clp_start + clp_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *clp_start + clp_window.size());
    for (std::size_t i = 0; i < clp_window.size(); ++i) {
        const auto slot = *clp_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << clp_window[i]) {
            BOOST_TEST(runtime_spec.name == clp_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }

    const auto clp_tail_start =
        seeded_env.lookup(std::string(clp_tail_window.front()));
    BOOST_REQUIRE(clp_tail_start.has_value());
    BOOST_REQUIRE(core_env.size() > *clp_tail_start);
    BOOST_REQUIRE(seeded_env.size() > *clp_tail_start);
    {
        const auto& runtime_spec = core_env.specs()[*clp_tail_start];
        const auto& seeded_spec = seeded_env.specs()[*clp_tail_start];
        BOOST_TEST_CONTEXT(
            "slot " << *clp_tail_start
                    << " name=" << clp_tail_window.front()) {
            BOOST_TEST(runtime_spec.name == clp_tail_window.front());
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_aad_window_matches_seeded_slots) {
    constexpr std::array<std::string_view, 13> aad_window = {{
        "tape-new",
        "tape-start!",
        "tape-stop!",
        "tape-clear!",
        "tape-var",
        "tape-backward!",
        "tape-adjoint",
        "tape-primal",
        "tape-ref?",
        "tape-ref-index",
        "tape-size",
        "tape-ref-value-of",
        "tape-ref-value"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    const auto aad_start = seeded_env.lookup(std::string(aad_window.front()));
    BOOST_REQUIRE(aad_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *aad_start + aad_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *aad_start + aad_window.size());
    for (std::size_t i = 0; i < aad_window.size(); ++i) {
        const auto slot = *aad_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << aad_window[i]) {
            BOOST_TEST(runtime_spec.name == aad_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(core_stats_windows_match_seeded_slots) {
    constexpr std::array<std::string_view, 21> fact_table_window = {{
        "%fact-table?",
        "fact-table?",
        "%make-fact-table",
        "%fact-table-insert!",
        "%fact-table-insert-clause!",
        "%fact-table-delete-row!",
        "%fact-table-row-live?",
        "%fact-table-row-ground?",
        "%fact-table-row-rule",
        "%fact-table-set-predicate!",
        "%fact-table-predicate",
        "%fact-table-build-index!",
        "%fact-table-query",
        "%fact-table-group-count",
        "%fact-table-group-sum",
        "%fact-table-live-row-ids",
        "%fact-table-ref",
        "%fact-table-row-count",
        "%fact-table-column-names",
        "term-hash",
        "term-variant-hash"
    }};

    constexpr std::array<std::string_view, 13> stats_window = {{
        "%stats-mean",
        "%stats-variance",
        "%stats-stddev",
        "%stats-sem",
        "%stats-percentile",
        "%stats-covariance",
        "%stats-correlation",
        "%stats-t-cdf",
        "%stats-t-quantile",
        "%stats-normal-quantile",
        "%stats-ci",
        "%stats-t-test-2",
        "%stats-ols"
    }};

    BuiltinEnvironment seeded_env;
    register_builtin_specs(seeded_env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment core_env;
    register_core_primitives(core_env, heap, intern, &vm);

    const auto fact_start =
        seeded_env.lookup(std::string(fact_table_window.front()));
    BOOST_REQUIRE(fact_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *fact_start + fact_table_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *fact_start + fact_table_window.size());
    for (std::size_t i = 0; i < fact_table_window.size(); ++i) {
        const auto slot = *fact_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << fact_table_window[i]) {
            BOOST_TEST(runtime_spec.name == fact_table_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }

    const auto stats_start = seeded_env.lookup(std::string(stats_window.front()));
    BOOST_REQUIRE(stats_start.has_value());
    BOOST_REQUIRE(core_env.size() >= *stats_start + stats_window.size());
    BOOST_REQUIRE(seeded_env.size() >= *stats_start + stats_window.size());
    for (std::size_t i = 0; i < stats_window.size(); ++i) {
        const auto slot = *stats_start + i;
        const auto& runtime_spec = core_env.specs()[slot];
        const auto& seeded_spec = seeded_env.specs()[slot];
        BOOST_TEST_CONTEXT("slot " << slot << " name=" << stats_window[i]) {
            BOOST_TEST(runtime_spec.name == stats_window[i]);
            BOOST_TEST(runtime_spec.name == seeded_spec.name);
            BOOST_TEST(runtime_spec.arity == seeded_spec.arity);
            BOOST_TEST(runtime_spec.has_rest == seeded_spec.has_rest);
            BOOST_TEST(static_cast<bool>(runtime_spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(runtime_builtin_registration_matches_metadata_exhaustive) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    const auto metadata = builtin_metadata();
    BOOST_REQUIRE_EQUAL(env.size(), metadata.size());

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    env.begin_patching();
    eta::interpreter::register_all_primitives(env, heap, intern, vm);
    env.verify_all_patched();

    BOOST_REQUIRE_EQUAL(env.size(), metadata.size());
    for (std::size_t i = 0; i < metadata.size(); ++i) {
        const auto& spec = env.specs()[i];
        const auto& doc = metadata[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << spec.name) {
            BOOST_TEST(spec.name == doc.name);
            BOOST_TEST(spec.arity == doc.arity);
            BOOST_TEST(spec.has_rest == doc.has_rest);
            BOOST_TEST(static_cast<bool>(spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(register_all_primitives_installs_nng_sidecar_placeholders) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    env.begin_patching();
    eta::interpreter::register_all_primitives(env, heap, intern, vm);
    env.verify_all_patched();

    const auto send_idx = env.lookup("send!");
    BOOST_REQUIRE(send_idx.has_value());
    BOOST_REQUIRE(static_cast<bool>(env.specs()[*send_idx].func));

    const auto send_result = env.specs()[*send_idx].func({});
    BOOST_REQUIRE(!send_result.has_value());

    const auto* vm_error = std::get_if<error::VMError>(&send_result.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(
        vm_error->message.find("requires package dependency 'eta-nng'")
        != std::string::npos);
}

BOOST_AUTO_TEST_CASE(builtin_metadata_is_consistent_across_consumers) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    const auto metadata = builtin_metadata();
    BOOST_REQUIRE_EQUAL(env.size(), metadata.size());

    for (std::size_t i = 0; i < metadata.size(); ++i) {
        const auto& spec = env.specs()[i];
        const auto& doc = metadata[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << spec.name) {
            BOOST_TEST(spec.name == doc.name);
            BOOST_TEST(spec.arity == doc.arity);
            BOOST_TEST(spec.has_rest == doc.has_rest);
        }
    }
}

BOOST_AUTO_TEST_CASE(builtin_metadata_order_matches_catalog_order) {
    const auto catalog = builtin_catalog();
    const auto metadata = builtin_metadata();

    BOOST_REQUIRE_EQUAL(metadata.size(), catalog.size());
    for (std::size_t i = 0; i < metadata.size(); ++i) {
        const auto& entry = catalog[i];
        const auto& builtin = metadata[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << entry.name) {
            BOOST_TEST(builtin.name == entry.name);
            BOOST_TEST(builtin.arity == entry.arity);
            BOOST_TEST(builtin.has_rest == entry.has_rest);
        }
    }
}

BOOST_AUTO_TEST_CASE(catalog_owner_matches_native_sidecar_lookup) {
    constexpr std::string_view sidecar_prefix = "sidecar:";

    for (const auto& entry : builtin_catalog()) {
        std::optional<std::string_view> expected_package;
        const std::string_view owner = entry.owner;
        if (owner.starts_with(sidecar_prefix)) {
            expected_package = owner.substr(sidecar_prefix.size());
        }

        const auto actual_package = builtin_native_sidecar_package(entry.name);

        BOOST_TEST_CONTEXT("builtin: " << entry.name << " owner=" << entry.owner) {
            BOOST_TEST(actual_package.has_value() == expected_package.has_value());
            if (expected_package.has_value()) {
                BOOST_REQUIRE(actual_package.has_value());
                BOOST_TEST(*actual_package == *expected_package);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(all_configured_builtins_have_docs) {
    constexpr std::array<std::string_view, 0> allowed_missing{};
    const auto missing = missing_builtin_docs(allowed_missing);

    if (!missing.empty()) {
        std::string message = "Builtins with missing docs:";
        for (const auto& name : missing) message += " " + name;
        BOOST_FAIL(message);
    }

    for (const auto& builtin : builtin_metadata()) {
        BOOST_TEST_CONTEXT("builtin: " << builtin.name) {
            BOOST_TEST(!builtin.category.empty());
            BOOST_TEST(!builtin.signature.empty());
            BOOST_TEST(!builtin.summary.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(doc_metadata_has_no_duplicate_special_forms) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;

    for (const auto& entry : eta::reader::special_form_docs()) {
        if (!seen.insert(std::string(entry.name)).second) {
            duplicates.push_back(std::string(entry.name));
        }
    }
    if (!duplicates.empty()) {
        std::string message = "Duplicate special-form docs:";
        for (const auto& name : duplicates) message += " " + name;
        BOOST_FAIL(message);
    }
}

BOOST_AUTO_TEST_CASE(doc_metadata_has_no_duplicate_builtins) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;

    for (const auto& builtin : builtin_metadata()) {
        if (!seen.insert(builtin.name).second) {
            duplicates.push_back(builtin.name);
        }
    }
    if (!duplicates.empty()) {
        std::string message = "Duplicate builtin docs:";
        for (const auto& name : duplicates) message += " " + name;
        BOOST_FAIL(message);
    }
}

BOOST_AUTO_TEST_CASE(doc_metadata_has_no_special_form_builtin_name_collisions) {
    std::unordered_set<std::string> builtin_names;
    for (const auto& builtin : builtin_metadata()) {
        builtin_names.insert(builtin.name);
    }

    std::vector<std::string> unexpected;
    for (const auto& entry : eta::reader::special_form_docs()) {
        if (!builtin_names.contains(std::string(entry.name))) continue;
        if (eta::reader::is_allowed_special_form_builtin_collision(entry.name)) continue;
        unexpected.push_back(std::string(entry.name));
    }

    if (!unexpected.empty()) {
        std::string message = "Unexpected special-form/builtin collisions:";
        for (const auto& name : unexpected) message += " " + name;
        BOOST_FAIL(message);
    }
}

/**
 * Verify patch mode mechanics: begin_patching + register_builtin validates
 * metadata and installs funcs without aborting when metadata matches.
 */
BOOST_AUTO_TEST_CASE(patch_mode_basic_mechanics) {
    BuiltinEnvironment env;

    /// Pre-register two names with null funcs
    env.register_builtin("foo", 1, false, PrimitiveFunc{});
    env.register_builtin("bar", 2, true,  PrimitiveFunc{});

    BOOST_TEST(env.size() == 2u);
    BOOST_TEST(!env.specs()[0].func);  ///< null
    BOOST_TEST(!env.specs()[1].func);  ///< null

    /// Switch to patch mode
    env.begin_patching();

    /// Patch with real funcs (matching metadata)
    PrimitiveFunc foo_fn = [](std::span<const nanbox::LispVal>)
        -> std::expected<nanbox::LispVal, error::RuntimeError> {
        return nanbox::Nil;
    };
    PrimitiveFunc bar_fn = [](std::span<const nanbox::LispVal>)
        -> std::expected<nanbox::LispVal, error::RuntimeError> {
        return nanbox::True;
    };

    env.register_builtin("foo", 1, false, foo_fn);
    env.register_builtin("bar", 2, true,  bar_fn);

    /// Both should now have non-null funcs
    BOOST_TEST(static_cast<bool>(env.specs()[0].func));
    BOOST_TEST(static_cast<bool>(env.specs()[1].func));

    /// verify_all_patched should succeed (no abort)
    env.verify_all_patched();

    /// Size unchanged
    BOOST_TEST(env.size() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
