#include "eta/linter/cli/reporter.h"

#include <algorithm>
#include <sstream>

namespace eta::linter::cli {
    namespace {
        const char *severity_name(Severity severity) {
            switch (severity) {
                case Severity::Error: return "error";
                case Severity::Warning: return "warning";
                case Severity::Info: return "info";
            }
            return "warning";
        }
    }

    std::string pretty_report(const std::vector<FileReport> &reports, bool quiet) {
        std::vector<Diagnostic> diagnostics;
        std::vector<CoverageNote> coverage;
        for (const auto &report : reports) {
            for (const auto &diagnostic : report.diagnostics) {
                if (quiet && diagnostic.severity == Severity::Info) continue;
                diagnostics.push_back(diagnostic);
            }
            coverage.insert(coverage.end(), report.coverage.begin(), report.coverage.end());
        }
        std::sort(diagnostics.begin(), diagnostics.end(), [](const Diagnostic &a, const Diagnostic &b) {
            if (a.path != b.path) return a.path < b.path;
            if (a.located != b.located) return a.located;
            if (a.located && a.span.begin != b.span.begin) return a.span.begin < b.span.begin;
            if (a.owner != b.owner) return a.owner < b.owner;
            return a.code < b.code;
        });
        std::ostringstream out;
        for (const auto &diagnostic : diagnostics) {
            out << diagnostic.path;
            if (diagnostic.located) out << ':' << diagnostic.line << ':' << diagnostic.column;
            out << ": " << severity_name(diagnostic.severity) << '[' << diagnostic.code << "]: "
                << diagnostic.message << '\n';
        }
        for (const auto &note : coverage) {
            out << note.scope << ": coverage: " << note.reason;
            if (!note.rules.empty()) {
                out << " [";
                for (std::size_t i = 0; i < note.rules.size(); ++i) {
                    if (i) out << ',';
                    out << note.rules[i];
                }
                out << ']';
            }
            out << '\n';
        }
        return out.str();
    }
}



