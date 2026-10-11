#include "eta/linter/core/engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <set>
#include <new>
#include <numeric>

#include "eta/linter/core/registry.h"
#include "eta/linter/rules/rules.h"

namespace eta::linter {
    namespace {
        struct FormNode {
            cst::NodeId id{};
            cst::ByteSpan span{};
            std::vector<std::size_t> children;
            std::set<std::string> disable_codes;
            std::set<std::string> enable_codes;
            std::set<std::string> suppressed;
        };

        struct DirectiveIssue {
            std::string message;
            cst::ByteSpan span{};
        };

        struct SourceEdit {
            cst::ByteSpan span{};
            std::string expected;
            std::string replacement;
            std::string rule;
        };

        const char *severity_name(Severity severity) {
            switch (severity) {
                case Severity::Error: return "error";
                case Severity::Warning: return "warning";
                case Severity::Info: return "info";
            }
            return "warning";
        }

        std::string trim(std::string_view text) {
            const auto begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string_view::npos) return {};
            const auto end = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(begin, end - begin + 1));
        }

        std::vector<std::string> split_codes(std::string_view text) {
            std::vector<std::string> codes;
            std::size_t start = 0;
            while (start <= text.size()) {
                const auto pos = text.find(',', start);
                const auto token = trim(text.substr(start, pos == std::string_view::npos ? text.size() - start : pos - start));
                if (!token.empty()) codes.push_back(token);
                if (pos == std::string_view::npos) break;
                start = pos + 1;
            }
            return codes;
        }

        bool contains(const cst::ByteSpan &outer, const cst::ByteSpan &inner) {
            return outer.begin <= inner.begin && inner.end <= outer.end;
        }

        bool intersects(const cst::ByteSpan &a, const cst::ByteSpan &b) {
            return a.begin < b.end && b.begin < a.end;
        }

        std::vector<FormNode> build_form_tree(const cst::File &file, const std::vector<rules::ActiveForm> &forms) {
            std::vector<FormNode> nodes;
            nodes.reserve(forms.size());
            for (const auto &form : forms) nodes.push_back({form.id, rules::node_extent(file, file.nodes[form.id])});
            std::vector<std::size_t> order(nodes.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                if (nodes[a].span.begin != nodes[b].span.begin) return nodes[a].span.begin < nodes[b].span.begin;
                if (nodes[a].span.end != nodes[b].span.end) return nodes[a].span.end > nodes[b].span.end;
                return nodes[a].id < nodes[b].id;
            });
            std::vector<std::size_t> stack;
            for (const auto index : order) {
                while (!stack.empty() && !contains(nodes[stack.back()].span, nodes[index].span)) stack.pop_back();
                if (!stack.empty()) nodes[stack.back()].children.push_back(index);
                stack.push_back(index);
            }
            return nodes;
        }

        std::optional<std::size_t> next_form_after(const std::vector<FormNode> &nodes, cst::ByteSpan span) {
            std::optional<std::size_t> target;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (nodes[i].span.begin < span.end) continue;
                if (!target || nodes[i].span.begin < nodes[*target].span.begin
                    || (nodes[i].span.begin == nodes[*target].span.begin && nodes[i].span.end < nodes[*target].span.end)) {
                    target = i;
                }
            }
            return target;
        }

        void collect_form_suppression(std::vector<FormNode> &nodes, std::size_t index, std::set<std::string> inherited) {
            auto current = std::move(inherited);
            current.insert(nodes[index].disable_codes.begin(), nodes[index].disable_codes.end());
            for (const auto &code : nodes[index].enable_codes) current.erase(code);
            nodes[index].suppressed = current;
            for (const auto child : nodes[index].children) collect_form_suppression(nodes, child, current);
        }

        std::optional<std::size_t> deepest_form_containing(const std::vector<FormNode> &nodes, std::size_t index, cst::ByteSpan span) {
            if (!contains(nodes[index].span, span)) return std::nullopt;
            for (const auto child : nodes[index].children)
                if (auto nested = deepest_form_containing(nodes, child, span)) return nested;
            return index;
        }

        bool suppressed_by_state(const std::vector<FormNode> &nodes, const std::set<std::string> &file_suppressed,
            const std::vector<std::pair<std::size_t, std::set<std::string>>> &line_suppressed, const Diagnostic &diagnostic) {
            if (!diagnostic.located) return file_suppressed.contains(diagnostic.code);
            for (const auto &entry : line_suppressed)
                if (entry.first == diagnostic.line && entry.second.contains(diagnostic.code)) return true;
            if (file_suppressed.contains(diagnostic.code)) return true;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (auto hit = deepest_form_containing(nodes, i, diagnostic.span)) {
                    return nodes[*hit].suppressed.contains(diagnostic.code);
                }
            }
            return false;
        }

        std::optional<std::string> parse_directive_kind(std::string_view text, std::vector<std::string> &codes) {
            const auto marker = text.find("eta-lint:");
            if (marker == std::string_view::npos) return std::nullopt;
            const auto payload = text.substr(marker + 9);
            const auto equals = payload.find('=');
            if (equals == std::string_view::npos) return std::string{};
            const auto kind = trim(payload.substr(0, equals));
            codes = split_codes(payload.substr(equals + 1));
            if (kind == "disable" || kind == "enable" || kind == "disable-line" || kind == "disable-file") return kind;
            return std::string{};
        }

        void add_directive_issue(std::vector<DirectiveIssue> &issues, std::string message, cst::ByteSpan span) {
            issues.push_back({std::move(message), span});
        }

        void parse_directives(const cst::File &file, const std::vector<rules::ActiveForm> &forms,
            std::set<std::string> &file_suppressed,
            std::vector<std::pair<std::size_t, std::set<std::string>>> &line_suppressed,
            std::vector<DirectiveIssue> &issues,
            std::vector<FormNode> &nodes) {
            if (forms.empty()) return;
            const auto first_form_begin = rules::node_extent(file, file.nodes[forms.front().id]).begin;
            for (const auto &association : file.associations) {
                const auto &trivia = file.trivia[association.comment];
                if (trivia.kind != cst::TriviaKind::LineComment) continue;
                if (association.kind == cst::CommentAssociation::DatumOperand || association.kind == cst::CommentAssociation::ReaderOperand) continue;
                const auto text = file.source->slice(trivia.span);
                std::vector<std::string> codes;
                const auto kind = parse_directive_kind(text, codes);
                if (!kind) continue;
                if (kind->empty()) {
                    add_directive_issue(issues, "invalid eta-lint directive syntax", trivia.span);
                    continue;
                }
                if (codes.empty()) {
                    add_directive_issue(issues, "eta-lint directive requires at least one rule code", trivia.span);
                    continue;
                }
                auto validate_codes = [&](const std::string &code) {
                    if (!find_rule(code)) {
                        add_directive_issue(issues, "unknown rule code in directive: " + code, trivia.span);
                        return false;
                    }
                    return true;
                };
                bool all_known = true;
                for (const auto &code : codes) all_known = validate_codes(code) && all_known;
                if (!all_known) continue;

                if (*kind == "disable-line") {
                    const auto pos = file.source->position(trivia.span.begin, cst::PositionEncoding::UnicodeScalar);
                    if (!pos) {
                        add_directive_issue(issues, "cannot resolve directive line", trivia.span);
                        continue;
                    }
                    std::set<std::string> values(codes.begin(), codes.end());
                    line_suppressed.emplace_back(pos->line + 1, std::move(values));
                    continue;
                }

                if (*kind == "disable-file") {
                    if (trivia.span.begin >= first_form_begin) {
                        add_directive_issue(issues, "eta-lint:disable-file must appear before the first active form", trivia.span);
                        continue;
                    }
                    file_suppressed.insert(codes.begin(), codes.end());
                    continue;
                }

                const auto target = next_form_after(nodes, trivia.span);
                if (!target) {
                    add_directive_issue(issues, "eta-lint directive has no following form to apply to", trivia.span);
                    continue;
                }
                auto &bucket = *kind == "enable" ? nodes[*target].enable_codes : nodes[*target].disable_codes;
                bucket.insert(codes.begin(), codes.end());
            }
        }

        void stamp_positions(const cst::Source &source, std::vector<Diagnostic> &diagnostics) {
            for (auto &diagnostic : diagnostics) {
                if (!diagnostic.located) continue;
                const auto position = source.position(diagnostic.span.begin, cst::PositionEncoding::UnicodeScalar);
                if (position) {
                    diagnostic.line = position->line + 1;
                    diagnostic.column = position->character + 1;
                }
            }
        }

        void filter_suppressed_diagnostics(std::vector<Diagnostic> &diagnostics, const std::vector<FormNode> &nodes,
            const std::set<std::string> &file_suppressed,
            const std::vector<std::pair<std::size_t, std::set<std::string>>> &line_suppressed) {
            std::vector<Diagnostic> kept;
            kept.reserve(diagnostics.size());
            for (auto &diagnostic : diagnostics) {
                if (diagnostic.code == "eta_lint.Directive") {
                    kept.push_back(std::move(diagnostic));
                    continue;
                }
                if (suppressed_by_state(nodes, file_suppressed, line_suppressed, diagnostic)) continue;
                kept.push_back(std::move(diagnostic));
            }
            diagnostics = std::move(kept);
        }

        std::string escape_json(std::string_view value) {
            std::string out;
            out.reserve(value.size());
            for (const char ch : value) {
                switch (ch) {
                    case '\\': out += "\\\\"; break;
                    case '"': out += "\\\""; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (static_cast<unsigned char>(ch) < 0x20) {
                            std::array<char, 8> buffer{};
                            std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                                static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                            out += buffer.data();
                        } else {
                            out.push_back(ch);
                        }
                        break;
                }
            }
            return out;
        }

        bool diagnostic_order(const Diagnostic &a, const Diagnostic &b) {
            if (a.path != b.path) return a.path < b.path;
            if (a.located != b.located) return a.located;  // located before unlocated
            if (a.located && a.span.begin != b.span.begin) return a.span.begin < b.span.begin;
            if (a.owner != b.owner) return a.owner < b.owner;
            return a.code < b.code;
        }

        std::vector<SourceEdit> collect_eta401_fixes(const cst::File &file, const std::vector<Diagnostic> &diagnostics) {
            std::vector<SourceEdit> edits;
            for (const auto &diagnostic : diagnostics) {
                if (diagnostic.code != "ETA401" || !diagnostic.located || diagnostic.span.begin >= diagnostic.span.end) continue;
                bool directive_comment = false;
                for (const auto &association : file.associations) {
                    const auto &comment = file.trivia[association.comment];
                    if (comment.kind != cst::TriviaKind::LineComment) continue;
                    const cst::ByteSpan markers{comment.span.begin, std::min(comment.span.end, diagnostic.span.end)};
                    if (markers == diagnostic.span) {
                        const auto text = file.source->slice(comment.span);
                        if (text.find("eta-lint:") != std::string_view::npos) directive_comment = true;
                        break;
                    }
                }
                if (directive_comment) continue;
                std::string replacement = ";";
                if (diagnostic.message.find("standalone") != std::string::npos) replacement = ";;";
                edits.push_back({diagnostic.span, std::string(file.source->slice(diagnostic.span)), replacement, "ETA401"});
            }
            return edits;
        }

        std::vector<SourceEdit> collect_eta205_fixes(const cst::File &file) {
            struct ImportForm {
                cst::ByteSpan span{};
                std::string_view module;
            };
            auto is_plain_import = [&](const cst::Node &node, std::string_view &module) {
                if (node.kind != cst::NodeKind::List || node.children.size() != 2 || node.discarded || node.verbatim) return false;
                const auto head = rules::symbol_text(file, file.nodes[node.children.front()]);
                if (head != "import") return false;
                module = rules::symbol_text(file, file.nodes[node.children[1]]);
                return !module.empty();
            };
            auto group_safe = [&](cst::ByteSpan span) {
                for (const auto &association : file.associations) {
                    const auto &comment = file.trivia[association.comment];
                    if ((comment.kind == cst::TriviaKind::LineComment || comment.kind == cst::TriviaKind::BlockComment)
                        && intersects(comment.span, span)) return false;
                }
                for (const auto &token : file.tokens) {
                    if (token.kind != cst::TokenKind::Directive) continue;
                    if (intersects(token.span, span)) return false;
                }
                return true;
            };

            std::vector<SourceEdit> edits;
            std::vector<ImportForm> group;
            auto flush = [&] {
                if (group.size() < 2) {
                    group.clear();
                    return;
                }
                const cst::ByteSpan span{group.front().span.begin, group.back().span.end};
                if (!group_safe(span)) {
                    group.clear();
                    return;
                }
                std::vector<std::size_t> order(group.size());
                std::iota(order.begin(), order.end(), 0);
                std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return group[a].module < group[b].module;
                });
                bool changed = false;
                for (std::size_t i = 0; i < order.size(); ++i) {
                    if (order[i] != i) {
                        changed = true;
                        break;
                    }
                }
                if (!changed) {
                    group.clear();
                    return;
                }
                std::vector<std::string_view> separators;
                separators.reserve(group.size() - 1);
                for (std::size_t i = 0; i + 1 < group.size(); ++i) {
                    separators.push_back(file.source->slice({group[i].span.end, group[i + 1].span.begin}));
                }
                std::string replacement;
                for (std::size_t i = 0; i < order.size(); ++i) {
                    replacement += file.source->slice(group[order[i]].span);
                    if (i + 1 < order.size()) replacement += separators[i];
                }
                edits.push_back({span, std::string(file.source->slice(span)), replacement, "ETA205"});
                group.clear();
            };

            for (const auto root : file.roots) {
                const auto &node = file.nodes[root];
                std::string_view module;
                if (is_plain_import(node, module)) group.push_back({rules::node_extent(file, node), module});
                else flush();
            }
            flush();
            return edits;
        }

        std::optional<Diagnostic> apply_fix_edits(const cst::Source &source, std::vector<SourceEdit> edits,
            std::string &output, std::string_view path) {
            auto conflict = [&](std::string message, cst::ByteSpan span, bool located = true) {
                return Diagnostic{"eta_lint.FixConflict", "eta_lint", Severity::Error,
                    std::move(message), span, located, 1, 1, std::string(path)};
            };
            for (const auto &edit : edits) {
                if (edit.span.begin > edit.span.end || edit.span.end > source.text().size()) {
                    return conflict("fix edit is outside the source buffer", {}, false);
                }
                if (!source.is_edit_boundary(edit.span.begin) || !source.is_edit_boundary(edit.span.end)) {
                    return conflict("fix edit would split UTF-8 or CRLF boundaries", edit.span);
                }
                if (source.slice(edit.span) != edit.expected) {
                    return conflict("fix edit expected bytes no longer match the analysed snapshot", edit.span);
                }
            }
            std::sort(edits.begin(), edits.end(), [](const SourceEdit &a, const SourceEdit &b) {
                if (a.span.begin != b.span.begin) return a.span.begin < b.span.begin;
                if (a.span.end != b.span.end) return a.span.end < b.span.end;
                if (a.replacement != b.replacement) return a.replacement < b.replacement;
                return a.rule < b.rule;
            });
            std::vector<SourceEdit> deduped;
            deduped.reserve(edits.size());
            for (const auto &edit : edits) {
                if (!deduped.empty() && deduped.back().span == edit.span && deduped.back().replacement == edit.replacement) continue;
                if (!deduped.empty() && deduped.back().span.end > edit.span.begin) {
                    return conflict("overlapping fix edits were generated for this file", {deduped.back().span.begin, edit.span.end});
                }
                deduped.push_back(edit);
            }
            output = std::string(source.text());
            for (auto it = deduped.rbegin(); it != deduped.rend(); ++it) {
                output.replace(it->span.begin, it->span.end - it->span.begin, it->replacement);
            }
            return std::nullopt;
        }

        int file_exit_class(const FileReport &report, const LintOptions &options) {
            int exit = report.exit_class;
            std::size_t warnings = 0;
            for (const auto &diagnostic : report.diagnostics) {
                if (diagnostic.severity == Severity::Error) exit = std::max(exit, 1);
                else if (diagnostic.severity == Severity::Warning) ++warnings;
            }
            if (options.max_warnings && warnings > *options.max_warnings) exit = std::max(exit, 1);
            return exit;
        }
    }

    FileReport lint_source(std::string source, std::string identity,
        const std::vector<std::string> &rules, const LintOptions &options, cst::Operation operation) {
        FileReport report;
        report.path = identity;
        operation.identity = identity;
        try {
            if (source.size() > operation.limits.input_bytes) {
                report.diagnostics.push_back({"cst.InputLimit", "cst", Severity::Error,
                    "input byte limit exceeded", {}, false, 1, 1, identity});
                report.exit_class = 2;
                return report;
            }
            auto parsed = cst::parse(std::move(source), cst::ReaderConfig{}, operation);
            if (!parsed) {
                report.diagnostics.push_back({parsed.error().code, "cst", Severity::Error,
                    parsed.error().message, {}, false, 1, 1, identity});
                report.exit_class = parsed.error().exit_class;
                return report;
            }
            if (parsed->status != cst::Status::Valid) {
                const bool recovered = parsed->status == cst::Status::Recovered;
                report.coverage.push_back({"file", recovered
                    ? "input fails the compiler reader; structural rules skipped"
                    : "valid but unsupported reader construct; structural rules skipped", rules});
                report.diagnostics.push_back({recovered ? "cst.Recovered" : "cst.Unsupported", "cst",
                    Severity::Error, recovered ? "input fails the compiler reader"
                    : "valid input uses an unsupported reader construct", {}, false, 1, 1, identity});
                report.exit_class = 2;
                return report;
            }
            const auto &file = *parsed->file;
            const auto forms = rules::collect_active_forms(file);
            auto form_nodes = build_form_tree(file, forms);
            std::set<std::string> file_suppressed;
            std::vector<std::pair<std::size_t, std::set<std::string>>> line_suppressed;
            std::vector<DirectiveIssue> directive_issues;
            parse_directives(file, forms, file_suppressed, line_suppressed, directive_issues, form_nodes);
            std::vector<std::size_t> roots;
            for (std::size_t i = 0; i < form_nodes.size(); ++i) {
                bool nested = false;
                for (const auto &node : form_nodes)
                    if (std::find(node.children.begin(), node.children.end(), i) != node.children.end()) { nested = true; break; }
                if (!nested) roots.push_back(i);
            }
            for (const auto root : roots) collect_form_suppression(form_nodes, root, file_suppressed);
            rules::RuleContext context{file, identity, forms, report.diagnostics};
            const auto enabled = [&](std::string_view code) {
                return std::find(rules.begin(), rules.end(), code) != rules.end();
            };
            if (enabled("ETA104")) rules::run_eta104(context);
            if (enabled("ETA203")) rules::run_eta203(context);
            if (enabled("ETA205")) rules::run_eta205(context);
            if (enabled("ETA401")) rules::run_eta401(context);
            if (enabled("ETA501")) rules::run_eta501(context);
            for (const auto &issue : directive_issues) {
                report.diagnostics.push_back({"eta_lint.Directive", "eta_lint", Severity::Error, issue.message, issue.span, true, 1, 1, identity});
            }
            stamp_positions(*file.source, report.diagnostics);
            filter_suppressed_diagnostics(report.diagnostics, form_nodes, file_suppressed, line_suppressed);
            std::sort(report.diagnostics.begin(), report.diagnostics.end(), diagnostic_order);
            report.exit_class = directive_issues.empty() ? file_exit_class(report, options) : std::max(2, file_exit_class(report, options));
            if (options.fix && report.exit_class < 2) {
                std::vector<SourceEdit> edits;
                if (enabled("ETA401")) {
                    auto eta401 = collect_eta401_fixes(file, report.diagnostics);
                    edits.insert(edits.end(), eta401.begin(), eta401.end());
                }
                if (enabled("ETA205")) {
                    auto eta205 = collect_eta205_fixes(file);
                    edits.insert(edits.end(), eta205.begin(), eta205.end());
                }
                if (!edits.empty()) {
                    std::string fixed;
                    if (auto conflict = apply_fix_edits(*file.source, std::move(edits), fixed, identity)) {
                        report.diagnostics.push_back(*conflict);
                        stamp_positions(*file.source, report.diagnostics);
                        std::sort(report.diagnostics.begin(), report.diagnostics.end(), diagnostic_order);
                        report.exit_class = 2;
                        return report;
                    }
                    if (fixed != file.source->text()) {
                        auto recheck = options;
                        recheck.fix = false;
                        auto rerun = lint_source(fixed, identity, rules, recheck, operation);
                        if (rerun.exit_class < 70) rerun.fixed_source = std::move(fixed);
                         return rerun;
                    }
                }
            }
            return report;
        } catch (const cst::detail::OperationStopped &stop) {
            report.diagnostics.push_back({stop.failure.code, "cst", Severity::Error,
                stop.failure.message, {}, false, 1, 1, identity});
            report.exit_class = stop.failure.exit_class;
        } catch (const std::bad_alloc &) {
            report.diagnostics.push_back({"cst.MemoryLimit", "cst", Severity::Error,
                "linter allocation failed", {}, false, 1, 1, identity});
            report.exit_class = 2;
        } catch (const std::exception &error) {
            report.diagnostics.push_back({"eta_lint.Internal", "eta_lint", Severity::Error,
                error.what(), {}, false, 1, 1, identity});
            report.exit_class = 70;
        }
        return report;
    }

    std::string json_report(const std::vector<FileReport> &reports) {
        std::vector<Diagnostic> diagnostics;
        std::vector<CoverageNote> coverage;
        std::size_t errors = 0, warnings = 0, infos = 0;
        for (const auto &report : reports) {
            diagnostics.insert(diagnostics.end(), report.diagnostics.begin(), report.diagnostics.end());
            coverage.insert(coverage.end(), report.coverage.begin(), report.coverage.end());
        }
        std::sort(diagnostics.begin(), diagnostics.end(), diagnostic_order);
        for (const auto &diagnostic : diagnostics) {
            switch (diagnostic.severity) {
                case Severity::Error: ++errors; break;
                case Severity::Warning: ++warnings; break;
                case Severity::Info: ++infos; break;
            }
        }
        std::string out = "{\"version\":1,\"summary\":{\"files\":" + std::to_string(reports.size())
            + ",\"errors\":" + std::to_string(errors) + ",\"warnings\":" + std::to_string(warnings)
            + ",\"infos\":" + std::to_string(infos) + "},\"coverage\":[";
        for (std::size_t i = 0; i < coverage.size(); ++i) {
            if (i) out += ',';
            out += "{\"scope\":\"" + escape_json(coverage[i].scope) + "\",\"reason\":\""
                + escape_json(coverage[i].reason) + "\",\"rules\":[";
            for (std::size_t j = 0; j < coverage[i].rules.size(); ++j) {
                if (j) out += ',';
                out += "\"" + escape_json(coverage[i].rules[j]) + "\"";
            }
            out += "]}";
        }
        out += "],\"diagnostics\":[";
        for (std::size_t i = 0; i < diagnostics.size(); ++i) {
            const auto &diagnostic = diagnostics[i];
            if (i) out += ',';
            out += "{\"owner\":\"" + escape_json(diagnostic.owner) + "\",\"code\":\""
                + escape_json(diagnostic.code) + "\",\"severity\":\"" + severity_name(diagnostic.severity)
                + "\",\"message\":\"" + escape_json(diagnostic.message) + "\",\"path\":\""
                + escape_json(diagnostic.path) + "\",\"located\":" + (diagnostic.located ? "true" : "false");
            if (diagnostic.located) {
                out += ",\"span\":[" + std::to_string(diagnostic.span.begin) + ','
                    + std::to_string(diagnostic.span.end) + "],\"line\":" + std::to_string(diagnostic.line)
                    + ",\"column\":" + std::to_string(diagnostic.column);
            } else {
                out += ",\"span\":null,\"line\":null,\"column\":null";
            }
            out += '}';
        }
        out += "]}";
        return out;
    }

    std::string sarif_report(const std::vector<FileReport> &reports) {
        std::vector<Diagnostic> diagnostics;
        for (const auto &report : reports)
            diagnostics.insert(diagnostics.end(), report.diagnostics.begin(), report.diagnostics.end());
        std::sort(diagnostics.begin(), diagnostics.end(), diagnostic_order);
        std::string out = "{\"version\":\"2.1.0\",\"$schema\":\"https://json.schemastore.org/sarif-2.1.0.json\",\"runs\":[{\"tool\":{\"driver\":{\"name\":\"eta_lint\",\"informationUri\":\"https://github.com/eta-lang/eta\",\"rules\":[";
        for (std::size_t i = 0; i < diagnostics.size(); ++i) {
            const auto &diagnostic = diagnostics[i];
            if (i) out += ',';
            out += "{\"id\":\"" + escape_json(diagnostic.code) + "\",\"name\":\"" + escape_json(diagnostic.code)
                + "\",\"shortDescription\":{\"text\":\"" + escape_json(diagnostic.message) + "\"},\"defaultConfiguration\":{\"level\":\""
                + severity_name(diagnostic.severity) + "\"}}";
        }
        out += "]}},\"results\":[";
        for (std::size_t i = 0; i < diagnostics.size(); ++i) {
            const auto &diagnostic = diagnostics[i];
            if (i) out += ',';
            out += "{\"ruleId\":\"" + escape_json(diagnostic.code) + "\",\"level\":\"" + severity_name(diagnostic.severity)
                + "\",\"message\":{\"text\":\"" + escape_json(diagnostic.message) + "\"},\"locations\":[{";
            out += "\"physicalLocation\":{\"artifactLocation\":{\"uri\":\"" + escape_json(diagnostic.path) + "\"}";
            if (diagnostic.located) {
                out += ",\"region\":{\"startLine\":" + std::to_string(diagnostic.line) + ",\"startColumn\":"
                    + std::to_string(diagnostic.column) + ",\"startOffset\":" + std::to_string(diagnostic.span.begin)
                    + ",\"endOffset\":" + std::to_string(diagnostic.span.end) + "}";
            }
            out += "}}]}";
        }
        out += "]}]}";
        return out;
    }

    std::string github_report(const std::vector<FileReport> &reports) {
        std::vector<Diagnostic> diagnostics;
        for (const auto &report : reports)
            diagnostics.insert(diagnostics.end(), report.diagnostics.begin(), report.diagnostics.end());
        std::sort(diagnostics.begin(), diagnostics.end(), diagnostic_order);
        std::string out;
        for (const auto &diagnostic : diagnostics) {
            out += "::";
            out += diagnostic.severity == Severity::Error ? "error" : diagnostic.severity == Severity::Warning ? "warning" : "notice";
            out += " file=" + escape_json(diagnostic.path);
            if (diagnostic.located) {
                out += ",line=" + std::to_string(diagnostic.line) + ",col=" + std::to_string(diagnostic.column);
            }
            out += "::" + escape_json(diagnostic.code + ": " + diagnostic.message) + '\n';
        }
        return out;
    }
}
