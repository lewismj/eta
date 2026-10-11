#include "parse.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "bridge_sexpr.h"
#include "eta/diagnostic/diagnostic.h"

namespace eta::cst {
    namespace {
        using Raw = reader::lexer::RawItem;
        using Kind = reader::lexer::Token::Kind;

        using Stop = detail::OperationStopped;

        bool opener(Kind kind) {
            return kind == Kind::LParen || kind == Kind::LBracket ||
                   kind == Kind::VectorStart || kind == Kind::ByteVectorStart;
        }

        bool closer(Kind kind) { return kind == Kind::RParen || kind == Kind::RBracket; }

        bool prefix(Kind kind) {
            return kind == Kind::Quote || kind == Kind::Backtick || kind == Kind::Comma || kind == Kind::CommaAt;
        }

        ByteSpan bytes(reader::lexer::Span span) {
            return {static_cast<std::size_t>(span.start.offset), static_cast<std::size_t>(span.end.offset)};
        }

        DiagnosticContext diagnostic_context(const File &file) {
            return {"reader", "", [&file](auto id) { return id == file.source->id() ? file.source : nullptr; }, true};
        }

        AdaptedDiagnostic cst_diagnostic(const File &file, std::string code, std::optional<ByteSpan> span,
                                         std::string message) {
            AdaptedDiagnostic result;
            result.source = "cst";
            result.producer = "eta_cst";
            result.code = std::move(code);
            if (!file.source->identity().empty()) result.location.file = file.source->identity();
            result.location.span = span;
            result.message = std::move(message);
            result.exit_class = 2;
            return result;
        }
    }

    class Storage final : public std::pmr::memory_resource {
    public:
        Storage(std::size_t base, const Operation &operation)
            : used_(base), peak_(base), limit_(operation.limits.memory_bytes), cancelled_(operation.cancelled) {
            check();
        }

        void check() const {
            if (cancelled_ && cancelled_->load(std::memory_order_relaxed))
                throw Stop{{"cst.Cancelled", "operation cancelled", 2}};
            if (used_ > limit_) throw Stop{{"cst.MemoryLimit", "tooling memory limit exceeded", 2}};
        }

        std::size_t peak() const { return peak_; }

        void reader_budget(std::size_t bytes) const {
            check();
            if (bytes > limit_ - used_) throw Stop{{"cst.MemoryLimit", "reader memory budget exceeded", 2}};
            peak_ = std::max(peak_, used_ + bytes);
        }

        void external(std::size_t bytes) {
            reader_budget(bytes);
            used_ += bytes;
        }

    private:
        void *do_allocate(std::size_t bytes, std::size_t alignment) override {
            check();
            if (bytes > limit_ - used_) throw Stop{{"cst.MemoryLimit", "tooling memory limit exceeded", 2}};
            auto *result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
            used_ += bytes;
            peak_ = std::max(peak_, used_);
            return result;
        }

        void do_deallocate(void *pointer, std::size_t bytes, std::size_t alignment) override {
            std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
            used_ -= bytes;
        }

        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }
        std::size_t used_;
        mutable std::size_t peak_;
        std::size_t limit_;
        std::shared_ptr<const std::atomic_bool> cancelled_;
    };

    File::File(std::shared_ptr<const Source> snapshot, ReaderConfig reader_config, Operation opts)
        : storage_(std::make_shared<Storage>(snapshot->memory_bytes() + sizeof(File) + sizeof(Storage)
                                             + opts.identity.capacity() + 128, opts)),
          source(std::move(snapshot)), config(reader_config), operation(std::move(opts)),
          tokens(storage_.get()), trivia(storage_.get()), nodes(storage_.get()), roots(storage_.get()),
          associations(storage_.get()), trace(storage_.get()) {
    }

    File::~File() = default;

    std::pmr::memory_resource *File::resource() const { return storage_.get(); }
    void File::checkpoint() const { storage_->check(); }
    std::size_t File::peak_memory_bytes() const { return storage_->peak(); }

    void File::check_reader_budget() const {
        storage_->reader_budget(tokens.size() * (sizeof(reader::parser::SExpr) + 4 * sizeof(void *) + 128)
                                + source->text().size() * 4 + 64 * 1024);
    }

    void File::check_transient_budget(std::size_t bytes) const { storage_->reader_budget(bytes); }
    void File::account_external_bytes(std::size_t bytes) const { storage_->external(bytes); }

    namespace {
        class Builder {
        public:
            Builder(File &file, ParseResult &result) : file_(file), result_(result), discards_(file.resource()) {
            }

            void scan() {
                const auto source = file_.source->text();
                file_.check_transient_budget(source.size() * 4 + 64 * 1024);
                // Eta currently rejects a BOM. Retain its exact header ownership even
                // for this compiler-invalid input; never strip it before validation.
                if (source.starts_with("\xEF\xBB\xBF")) add_token(TokenKind::FileBom, {0, 3});
                reader::lexer::Lexer lexer(file_.source->id(), source);
                reader::lexer::ScanObserver observer;
                observer.checkpoint = [&] { file_.checkpoint(); };
                observer.item = [&](const Raw &item) { raw_item(item); };
                observer.discarded = [&](reader::lexer::Span, bool complete) {
                    const auto id = discards_.back();
                    discards_.pop_back();
                    file_.tokens[id].discarded_end = file_.tokens.size();
                    file_.tokens[id].discard_complete = complete;
                };
                for (;;) {
                    file_.checkpoint();
                    auto token = lexer.scan_next(observer);
                    if (!token) {
                        result_.status = Status::Recovered;
                        file_.account_external_bytes(
                            2 * (sizeof(AdaptedDiagnostic) + token.error().message.size() + 256));
                        result_.diagnostics.push_back(adapt(token.error(), diagnostic_context(file_)));
                        if (!file_.tokens.empty() && file_.tokens.back().kind == TokenKind::Eof) {
                            pending_trivia_ = file_.tokens.back().trivia_begin;
                            file_.tokens.pop_back();
                        }
                        const auto begin = covered_end();
                        add_token(TokenKind::Error, {begin, source.size()});
                        add_token(TokenKind::Eof, {source.size(), source.size()});
                        break;
                    }
                    if (token->kind == Kind::EOF_) break;
                }
            }

            void structure() {
                while (cursor_ < file_.tokens.size() && file_.tokens[cursor_].kind != TokenKind::Eof) {
                    if (file_.tokens[cursor_].kind == TokenKind::FileBom || file_.tokens[cursor_].kind ==
                        TokenKind::Shebang) {
                        ++cursor_;
                        continue;
                    }
                    file_.roots.push_back(item(false, 0));
                }
                associate();
                for (const auto &token: file_.tokens) {
                    TraceEntry entry(file_.resource());
                    entry.token = token.id;
                    file_.trace.push_back(std::move(entry));
                }
                std::pmr::vector<TokenId> path(file_.resource());
                for (const auto root: file_.roots) trace_node(root, path, 0);
            }

        private:
            File &file_;
            ParseResult &result_;
            std::pmr::vector<TokenId> discards_;
            std::size_t pending_trivia_{};
            std::size_t cursor_{};
            std::size_t lexical_depth_{};
            std::size_t prefix_depth_{};

            std::size_t covered_end() const {
                const auto token_end = file_.tokens.empty() ? 0 : file_.tokens.back().span.end;
                const auto trivia_end = file_.trivia.empty() ? 0 : file_.trivia.back().span.end;
                return std::max(token_end, trivia_end);
            }

            TokenId add_token(TokenKind kind, ByteSpan span, std::optional<Kind> reader_kind = {}) {
                file_.checkpoint();
                if (file_.tokens.size() >= file_.operation.limits.tokens)
                    throw Stop{{"cst.TokenLimit", "concrete token limit exceeded", 2}};
                const auto id = file_.tokens.size();
                Token token;
                token.id = id;
                token.kind = kind;
                token.reader_kind = reader_kind;
                token.span = span;
                token.trivia_begin = pending_trivia_;
                token.trivia_end = file_.trivia.size();
                pending_trivia_ = token.trivia_end;
                token.before.strict_quasiquote = token.after.strict_quasiquote = file_.config.strict_quasiquote;
                file_.tokens.push_back(token);
                return id;
            }

            void raw_item(const Raw &raw) {
                const auto span = bytes(raw.span);
                if (raw.kind == Raw::Kind::Whitespace || raw.kind == Raw::Kind::LineComment || raw.kind ==
                    Raw::Kind::BlockComment) {
                    file_.trivia.push_back({
                        file_.trivia.size(),
                        raw.kind == Raw::Kind::Whitespace
                            ? TriviaKind::Whitespace
                            : raw.kind == Raw::Kind::LineComment
                                  ? TriviaKind::LineComment
                                  : TriviaKind::BlockComment,
                        span
                    });
                    return;
                }
                TokenKind kind = TokenKind::Reader;
                if (raw.kind == Raw::Kind::DatumComment) kind = TokenKind::DatumComment;
                else if (raw.kind == Raw::Kind::Directive) kind = TokenKind::Directive;
                else if (raw.kind == Raw::Kind::Shebang) kind = TokenKind::Shebang;
                else if (raw.token && raw.token->kind == Kind::EOF_) kind = TokenKind::Eof;
                // The lexer historically includes CR in line-comment/shebang bodies.
                // Ownership exposes a CRLF terminator as whitespace without changing
                // the compiler token stream or the recognised shebang boundary.
                auto concrete_span = span;
                if (kind == TokenKind::Shebang && span.end > span.begin && file_.source->text()[span.end - 1] == '\r'
                    && span.end < file_.source->text().size() && file_.source->text()[span.end] == '\n')
                    --concrete_span.end;
                const auto id = add_token(kind, concrete_span,
                                          raw.token ? std::optional(raw.token->kind) : std::nullopt);
                file_.tokens[id].before.at_bof = raw.at_bof_before;
                file_.tokens[id].after.at_bof = raw.at_bof_after;
                file_.tokens[id].before.shebang_eligible = raw.at_bof_before && raw.span.start.line == 1 && raw.span.start.column == 1;
                file_.tokens[id].after.shebang_eligible = raw.at_bof_after && raw.span.end.line == 1 && raw.span.end.column == 1;
                if (concrete_span.end != span.end)
                    file_.trivia.push_back({
                        file_.trivia.size(), TriviaKind::Whitespace, {concrete_span.end, span.end}
                    });
                if (kind == TokenKind::DatumComment) discards_.push_back(id);
                if (raw.token) {
                    const auto token_kind = raw.token->kind;
                    if (opener(token_kind)) {
                        ++lexical_depth_;
                        prefix_depth_ = 0;
                    } else if (closer(token_kind)) {
                        if (lexical_depth_) --lexical_depth_;
                        prefix_depth_ = 0;
                    } else if (prefix(token_kind)) ++prefix_depth_;
                    else prefix_depth_ = 0;
                }
                if (lexical_depth_ + prefix_depth_ + discards_.size() > file_.operation.limits.depth)
                    throw Stop{{"cst.DepthLimit", "reader nesting limit exceeded", 2}};
            }

            NodeId add_node(Node node) {
                file_.checkpoint();
                if (file_.nodes.size() >= file_.operation.limits.nodes)
                    throw Stop{{"cst.NodeLimit", "CST node limit exceeded", 2}};
                const auto id = file_.nodes.size();
                file_.nodes.push_back(std::move(node));
                return id;
            }

            void recover(Node &node, std::string message) {
                node.verbatim = true;
                result_.status = Status::Recovered;
                file_.account_external_bytes(2 * (sizeof(AdaptedDiagnostic) + message.size() + 256));
                result_.diagnostics.push_back(cst_diagnostic(file_, "cst.Recovery", file_.tokens[node.token_begin].span,
                                                             std::move(message)));
            }

            NodeId item(bool discarded, std::size_t depth, std::optional<TokenId> boundary = {}) {
                file_.checkpoint();
                if (depth > file_.operation.limits.depth) throw Stop{
                    {"cst.DepthLimit", "CST nesting limit exceeded", 2}
                };
                Node node(file_.resource());
                node.token_begin = cursor_;
                node.discarded = discarded;
                const auto token = file_.tokens[cursor_++];
                if (token.kind == TokenKind::DatumComment) {
                    node.kind = NodeKind::DatumComment;
                    node.verbatim = true;
                    const auto end = token.discarded_end.value_or(cursor_);
                    while (cursor_ < end) node.children.push_back(item(true, depth + 1, end));
                    if (!token.discard_complete) recover(node, "incomplete discarded datum");
                } else if (token.kind == TokenKind::Directive) {
                    node.kind = NodeKind::Directive;
                } else if (token.kind == TokenKind::Error) {
                    node.kind = NodeKind::Error;
                    node.verbatim = true;
                } else if (token.reader_kind && opener(*token.reader_kind)) {
                    const auto kind = *token.reader_kind;
                    node.kind = kind == Kind::VectorStart
                                    ? NodeKind::VectorLiteral
                                    : kind == Kind::ByteVectorStart
                                          ? NodeKind::BytevectorLiteral
                                          : NodeKind::List;
                    const auto close = kind == Kind::LBracket ? Kind::RBracket : Kind::RParen;
                    bool closed = false;
                    while (cursor_ < file_.tokens.size() && (!boundary || cursor_ < *boundary)) {
                        const auto &next = file_.tokens[cursor_];
                        if (next.kind == TokenKind::Eof) break;
                        if (next.reader_kind && closer(*next.reader_kind)) {
                            // skip_datum accepts mismatched nested delimiter pairs;
                            // retain them in suppressed regions without active grammar.
                            if (*next.reader_kind == close || discarded) {
                                ++cursor_;
                                closed = true;
                            }
                            break;
                        }
                        if (next.reader_kind == Kind::Dot && node.kind == NodeKind::List) {
                            if (node.dot && !discarded) recover(node, "multiple dotted-list dots");
                            node.dot = cursor_++;
                            while (cursor_ < file_.tokens.size() && (!boundary || cursor_ < *boundary) && (
                                       file_.tokens[cursor_].kind == TokenKind::Directive ||
                                       file_.tokens[cursor_].kind == TokenKind::DatumComment))
                                node.children.push_back(item(discarded, depth + 1, boundary));
                            if (cursor_ < file_.tokens.size() && (!boundary || cursor_ < *boundary) && file_.tokens[
                                    cursor_].kind != TokenKind::Eof &&
                                !(file_.tokens[cursor_].reader_kind && closer(*file_.tokens[cursor_].reader_kind)))
                                node.tail = item(discarded, depth + 1, boundary);
                            else if (!discarded) recover(node, "missing dotted-list tail");
                        } else node.children.push_back(item(discarded, depth + 1, boundary));
                    }
                    if (!closed) {
                        node.virtual_close = ByteSpan{
                            file_.tokens[cursor_].span.begin, file_.tokens[cursor_].span.begin
                        };
                        if (!discarded) recover(node, "missing matching delimiter");
                        else node.verbatim = true;
                    }
                } else if (token.reader_kind && prefix(*token.reader_kind)) {
                    node.kind = NodeKind::ReaderForm;
                    while (cursor_ < file_.tokens.size() && (!boundary || cursor_ < *boundary) &&
                           (file_.tokens[cursor_].kind == TokenKind::DatumComment || file_.tokens[cursor_].kind ==
                            TokenKind::Directive))
                        node.children.push_back(item(discarded, depth + 1, boundary));
                    if (cursor_ < file_.tokens.size() && (!boundary || cursor_ < *boundary) &&
                        file_.tokens[cursor_].kind != TokenKind::Eof &&
                        !(file_.tokens[cursor_].reader_kind && closer(*file_.tokens[cursor_].reader_kind)))
                        node.children.push_back(item(discarded, depth + 1, boundary));
                    else if (!discarded) recover(node, "missing reader-form operand");
                } else if (token.reader_kind && (closer(*token.reader_kind) || *token.reader_kind == Kind::Dot)) {
                    node.kind = NodeKind::Error;
                    if (!discarded) recover(node, "unexpected delimiter or dot");
                    else node.verbatim = true;
                }
                node.token_end = cursor_;
                return add_node(std::move(node));
            }

            void associate() {
                for (const auto &token: file_.tokens) {
                    for (auto i = token.trivia_begin; i < token.trivia_end; ++i) {
                        const auto &trivia = file_.trivia[i];
                        if (trivia.kind == TriviaKind::Whitespace) continue;
                        auto kind = CommentAssociation::Leading;
                        TokenId anchor = token.id;
                        if (token.id == 0 || (token.id <= 2 &&
                                              (file_.tokens[token.id - 1].kind == TokenKind::Shebang || file_.tokens[
                                                   token.id - 1].kind == TokenKind::FileBom)))
                            kind = CommentAssociation::Header;
                        else if (token.id && (file_.tokens[token.id - 1].kind == TokenKind::DatumComment ||
                                              (file_.tokens[token.id - 1].reader_kind && prefix(
                                                   *file_.tokens[token.id - 1].reader_kind)))) {
                            anchor = token.id - 1;
                            kind = file_.tokens[anchor].kind == TokenKind::DatumComment
                                       ? CommentAssociation::DatumOperand
                                       : CommentAssociation::ReaderOperand;
                        } else if (token.reader_kind && closer(*token.reader_kind) && token.id &&
                                   file_.tokens[token.id - 1].reader_kind && opener(
                                       *file_.tokens[token.id - 1].reader_kind))
                            kind = CommentAssociation::Dangling;
                        else if (token.id && file_.source->position(file_.tokens[token.id - 1].span.end,
                                                                    PositionEncoding::UnicodeScalar)->line ==
                                 file_.source->position(trivia.span.begin, PositionEncoding::UnicodeScalar)->line) {
                            kind = CommentAssociation::EndOfLine;
                            anchor = token.id - 1;
                        } else if (token.kind == TokenKind::Eof || (token.reader_kind && closer(*token.reader_kind)))
                            kind = CommentAssociation::Dangling;
                        file_.associations.push_back({i, anchor, kind});
                    }
                }
            }

            void trace_node(NodeId id, std::pmr::vector<TokenId> &path, std::size_t qq_depth) {
                file_.checkpoint();
                const auto &node = file_.nodes[id];
                file_.trace[node.token_begin].path = path;
                auto &token = file_.tokens[node.token_begin];
                token.before.quasiquote_depth = qq_depth;
                token.after.quasiquote_depth = qq_depth;
                if (token.reader_kind == Kind::Backtick && !node.discarded) ++qq_depth;
                path.push_back(node.token_begin);
                for (auto i = node.token_begin + 1; i < node.token_end; ++i) {
                    file_.trace[i].path = path;
                    file_.tokens[i].before.quasiquote_depth = qq_depth;
                    file_.tokens[i].after.quasiquote_depth = qq_depth;
                }
                for (const auto child: node.children) trace_node(child, path, qq_depth);
                if (node.tail) trace_node(*node.tail, path, qq_depth);
                path.pop_back();
            }
        };
    }

    std::expected<ParseResult, Failure> parse(std::string text, ReaderConfig config, Operation operation) {
        try {
            if (config.grammar != Grammar::EtaCurrent) return std::unexpected(Failure{
                "cst.ReaderConfig", "unknown reader grammar", 2
            });
            if (operation.cancelled && operation.cancelled->load(std::memory_order_relaxed))
                return std::unexpected(Failure{"cst.Cancelled", "operation cancelled", 2});
            if (text.size() > operation.limits.input_bytes) return std::unexpected(Failure{
                "cst.InputLimit", "input byte limit exceeded", 2
            });
            // Bound the line map and snapshot before allocating either.
            const auto checkpoint = [&] {
                if (operation.cancelled && operation.cancelled->load(std::memory_order_relaxed))
                    throw Stop{{"cst.Cancelled", "operation cancelled", 2}};
            };
            std::size_t lines = 1;
            for (std::size_t i = 0; i < text.size(); i += 4096) {
                checkpoint();
                lines += std::count(text.begin() + i, text.begin() + std::min(i + 4096, text.size()), '\n');
            }
            if (text.capacity() + operation.identity.capacity() + Source::map_memory_bound(text.size(), lines) + sizeof(Source) + sizeof(
                    File) + 256 > operation.limits.memory_bytes)
                return std::unexpected(Failure{"cst.MemoryLimit", "source memory limit exceeded", 2});
            auto snapshot = std::make_shared<const Source>(operation.file_id, operation.identity, std::move(text),
                                                           operation.document_version, checkpoint);
            auto file = std::make_shared<File> (std::move(snapshot), config, std::move(operation));
            ParseResult result;
            Builder builder(*file, result);
            builder.scan();
            builder.structure();
            file->checkpoint();
            file->check_reader_budget();
            auto original = read_original(*file, true);
            if (!original) {
                file->account_external_bytes(2 * (sizeof(AdaptedDiagnostic) + 1024));
                if (const auto *failure = std::get_if<Failure>(&original.error())) {
                    if (failure->code != "cst.ReaderDisagreement") return std::unexpected(*failure);
                    result.status = Status::Unsupported;
                    result.diagnostics.push_back(cst_diagnostic(*file, failure->code, {}, failure->message));
                } else {
                    result.status = Status::Recovered;
                    std::visit([&](const auto &error) {
                        auto finding = adapt(error, diagnostic_context(*file));
                        if (std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                                         [&](const auto &previous) { return same_diagnostic(previous, finding); }))
                            result.diagnostics.push_back(std::move(finding));
                    }, std::get<reader::parser::ReaderError>(original.error()));
                }
            }
            if (!file->source->valid_utf8()) {
                if (result.status == Status::Valid) result.status = Status::Unsupported;
                file->account_external_bytes(2 * (sizeof(AdaptedDiagnostic) + 256));
                result.diagnostics.push_back(cst_diagnostic(*file, "cst.InvalidUtf8", {},
                                                            "tooling requires valid UTF-8; original bytes retained"));
            }
            result.file = std::move(file);
            return result;
        } catch (const Stop &stop) { return std::unexpected(stop.failure); } catch (const std::bad_alloc &) {
            return std::unexpected(Failure{"cst.MemoryLimit", "allocation failed", 2});
        }
    }

    bool has_unique_byte_ownership(const File &file) {
        std::size_t offset = 0;
        std::size_t trivia_id = 0;
        for (const auto &token: file.tokens) {
            if (token.id != &token - file.tokens.data() || token.trivia_begin != trivia_id) return false;
            for (; trivia_id < token.trivia_end; ++trivia_id) {
                if (trivia_id >= file.trivia.size()) return false;
                const auto &item = file.trivia[trivia_id];
                if (item.id != trivia_id || item.span.begin != offset || item.span.end < offset) return false;
                offset = item.span.end;
            }
            if (token.span.begin != offset || token.span.end < offset || token.span.end > file.source->text().size())
                return false;
            offset = token.span.end;
        }
        return !file.tokens.empty() && file.tokens.back().kind == TokenKind::Eof &&
               file.tokens.back().span == ByteSpan{file.source->text().size(), file.source->text().size()} &&
               trivia_id == file.trivia.size() && offset == file.source->text().size();
    }

    std::expected<std::string, Failure> serialize_losslessly(const File &file) {
        try {
            file.checkpoint();
            if (file.source->text().size() > file.operation.limits.output_bytes)
                return std::unexpected(Failure{"cst.OutputLimit", "output byte limit exceeded", 2});
            if (!has_unique_byte_ownership(file)) return std::unexpected(Failure{
                "cst.Ownership", "invalid concrete byte ownership", 70
            });
            file.check_transient_budget(file.source->text().size() + sizeof(std::string) + 1);
            std::string output;
            output.reserve(file.source->text().size());
            for (const auto &token: file.tokens) {
                file.checkpoint();
                for (auto i = token.trivia_begin; i < token.trivia_end; ++i) output.append(
                    file.source->slice(file.trivia[i].span));
                output.append(file.source->slice(token.span));
            }
            return output;
        } catch (const Stop &stop) { return std::unexpected(stop.failure); } catch (const std::bad_alloc &) {
            return std::unexpected(Failure{"cst.MemoryLimit", "allocation failed", 2});
        }
    }

    bool equivalent_reader_trace(const File &left, const File &right) {
        if (left.config != right.config || left.tokens.size() != right.tokens.size() || left.trace.size() != right.trace
            .size()) return false;
        for (std::size_t i = 0; i < left.tokens.size(); ++i) {
            const auto &a = left.tokens[i];
            const auto &b = right.tokens[i];
            if (a.kind != b.kind || a.reader_kind != b.reader_kind || a.before != b.before || a.after != b.after ||
                a.discarded_end != b.discarded_end || a.discard_complete != b.discard_complete ||
                left.source->slice(a.span) != right.source->slice(b.span))
                return false;
        }
        for (std::size_t i = 0; i < left.trace.size(); ++i)
            if (left.trace[i].token != right.trace[i].token || left.trace[i].path != right.trace[i].path) return false;
        return true;
    }
}
