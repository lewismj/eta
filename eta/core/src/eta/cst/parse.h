#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <memory_resource>
#include <vector>

#include "source.h"
#include "diagnostic.h"
#include "eta/reader/parser.h"

namespace eta::cst {
    using TokenId = std::size_t;
    using TriviaId = std::size_t;
    using NodeId = std::size_t;

    enum class Grammar : std::uint8_t { EtaCurrent };

    struct ReaderConfig {
        Grammar grammar{Grammar::EtaCurrent};
        bool strict_quasiquote{false};

        bool operator==(const ReaderConfig &) const = default;
    };

    struct Limits {
        std::size_t input_bytes{16 * 1024 * 1024};
        std::size_t output_bytes{32 * 1024 * 1024};
        std::size_t depth{256};
        std::size_t tokens{1'000'000};
        std::size_t nodes{1'000'000};
        std::size_t memory_bytes{256 * 1024 * 1024};
    };

    struct Operation {
        Limits limits{};
        std::shared_ptr<const std::atomic_bool> cancelled;
        reader::lexer::FileId file_id{1};
        std::string identity;
        std::optional<std::int64_t> document_version;
    };

    struct Failure {
        std::string code;
        std::string message;
        int exit_class{2};
    };

    namespace detail {
        struct OperationStopped {
            Failure failure;
        };
    }

    enum class TriviaKind : std::uint8_t { Whitespace, LineComment, BlockComment };

    struct Trivia {
        TriviaId id{};
        TriviaKind kind{};
        ByteSpan span{};
    };

    enum class TokenKind : std::uint8_t { Reader, DatumComment, Directive, Shebang, FileBom, Error, Eof };

    struct ReaderState {
        bool at_bof{true};
        bool strict_quasiquote{false};
        std::size_t quasiquote_depth{};
        bool shebang_eligible{true};

        bool operator==(const ReaderState &) const = default;
    };

    struct Token {
        TokenId id{};
        TokenKind kind{TokenKind::Reader};
        std::optional<reader::lexer::Token::Kind> reader_kind;
        ByteSpan span{};
        /// Half-open range in the file trivia arena; only this token owns it.
        TriviaId trivia_begin{};
        TriviaId trivia_end{};
        /// Exact boundary reported by Lexer::skip_datum, including nested comments.
        std::optional<TokenId> discarded_end;
        bool discard_complete{false};
        ReaderState before{};
        ReaderState after{};
    };

    enum class NodeKind : std::uint8_t {
        Atom, List, ReaderForm, VectorLiteral, BytevectorLiteral, DatumComment, Directive, Error
    };

    enum class Status : std::uint8_t { Valid, Recovered, Unsupported };

    struct Node {
        explicit Node(std::pmr::memory_resource *resource) : children(resource) {
        }

        NodeKind kind{NodeKind::Atom};
        TokenId token_begin{};
        TokenId token_end{};
        std::pmr::vector<NodeId> children;
        std::optional<TokenId> dot;
        std::optional<NodeId> tail;
        /// Missing close at this insertion point; it owns/emits no bytes.
        std::optional<ByteSpan> virtual_close;
        bool discarded{false};
        bool verbatim{false};
    };

    enum class CommentAssociation : std::uint8_t { Header, EndOfLine, Leading, Dangling, ReaderOperand, DatumOperand };

    struct Association {
        TriviaId comment{};
        TokenId anchor{};
        CommentAssociation kind{CommentAssociation::Leading};
    };

    struct TraceEntry {
        explicit TraceEntry(std::pmr::memory_resource *resource) : path(resource) {
        }

        TokenId token{};
        /// Opening/prefix token IDs, including all enclosing discarded input.
        std::pmr::vector<TokenId> path;
    };

    class Storage;

    class File {
    public:
        File(std::shared_ptr<const Source> source, ReaderConfig config, Operation operation);

        ~File();

        File(const File &) = delete;

        File &operator=(const File &) = delete;

        [[nodiscard]] std::pmr::memory_resource *resource() const;

        void checkpoint() const;

        [[nodiscard]] std::size_t peak_memory_bytes() const;

        /// Reserve a conservative upper bound for the original reader/bridge's
        /// ordinary allocations, before invoking either recursive operation.
        void check_reader_budget() const;

        void check_transient_budget(std::size_t bytes) const;

        void account_external_bytes(std::size_t bytes) const;

    private:
        std::shared_ptr<Storage> storage_;

    public:
        const std::shared_ptr<const Source> source;
        const ReaderConfig config;
        const Operation operation;
        std::pmr::vector<Token> tokens;
        std::pmr::vector<Trivia> trivia;
        std::pmr::vector<Node> nodes;
        std::pmr::vector<NodeId> roots;
        std::pmr::vector<Association> associations;
        std::pmr::vector<TraceEntry> trace;
    };

    struct ParseResult {
        std::shared_ptr<const File> file;
        std::vector<AdaptedDiagnostic> diagnostics;
        Status status{Status::Valid};
    };

    std::expected<ParseResult, Failure> parse(std::string source, ReaderConfig config, Operation operation = {});

    std::expected<std::string, Failure> serialize_losslessly(const File &file);

    bool has_unique_byte_ownership(const File &file);

    bool equivalent_reader_trace(const File &left, const File &right);
}
