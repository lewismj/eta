#pragma once

#include <string_view>
#include <vector>

#include "eta/cst/parse.h"
#include "eta/linter/lint.h"

namespace eta::linter::rules {
    /// An active (non-quoted, non-suppressed) list whose head is a symbol.
    struct ActiveForm {
        cst::NodeId id{};
        std::string_view head;
    };

    /// Shared read-only state passed to every structural rule. Rules append
    /// findings; severity is stamped from the registry by the engine.
    struct RuleContext {
        const cst::File &file;
        std::string_view path;
        const std::vector<ActiveForm> &forms;
        std::vector<Diagnostic> &out;
    };

    /// Raw byte span covering a node's concrete tokens (no leading trivia).
    cst::ByteSpan node_extent(const cst::File &file, const cst::Node &node);

    /// Raw spelling of a node when it is an atom symbol, else empty.
    std::string_view symbol_text(const cst::File &file, const cst::Node &node);

    /// Collect active forms in source order. Reader prefixes, explicit quote and
    /// quasiquote heads, vectors and suppressed datum subtrees are treated as
    /// data, so a quoted `define` is never mistaken for a real definition.
    std::vector<ActiveForm> collect_active_forms(const cst::File &file);

    void run_eta104(RuleContext &context);
    void run_eta203(RuleContext &context);
    void run_eta205(RuleContext &context);
    void run_eta401(RuleContext &context);
    void run_eta501(RuleContext &context);
}

