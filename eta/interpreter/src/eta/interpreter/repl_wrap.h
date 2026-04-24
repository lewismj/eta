#pragma once

#include <string>
#include <vector>

namespace eta::interpreter {

/**
 * @brief Metadata for a previously successful REPL submission module.
 */
struct PriorModule {
    std::string name;                  ///< Synthesized module name (e.g. "__repl_14")
    std::vector<std::string> exports;  ///< User-defined names exported by this module
};

/**
 * @brief Result of wrapping one REPL submission into a synthesized module.
 */
struct ReplWrapResult {
    std::string source;                ///< Full `(module ...)` source text to compile
    std::string module_name;           ///< Synthesized module name (e.g. "__repl_15")
    std::string result_name;           ///< Result binding name, or empty if no final expression
    std::vector<std::string> user_defines;
    bool last_is_expr{false};
};

/**
 * @brief Wrap parsed REPL forms into a module with selective imports.
 *
 * The wrapper preserves REPL shadowing semantics: newer definitions hide older
 * ones for future submissions by importing only the live names from prior
 * modules.
 */
ReplWrapResult wrap_repl_submission(
    const std::vector<std::string>& forms,
    int repl_id,
    bool prelude_available,
    const std::vector<PriorModule>& prior_modules);

} ///< namespace eta::interpreter

