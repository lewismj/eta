#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "eta/reader/parser.h"
#include "arena.h"

namespace eta::semantics::core {

using eta::reader::parser::Span;
using eta::reader::parser::SExprPtr;

/**
 * @brief Unique identifier for a binding (variable, parameter, etc.)
 */
struct BindingId {
    std::uint32_t id{};
    auto operator<=>(const BindingId&) const = default;
};

/**
 * @brief Address where a variable is read/written from after closure conversion
 *
 * This discriminated union represents where a variable can be found at runtime:
 * - Local: on the current stack frame
 * - Upval: captured in a closure's upvalue array
 * - Global: in the global environment
 */
struct Address {
    struct Local  { std::uint16_t slot{}; };
    struct Upval  { std::uint16_t slot{}; };
    struct Global { std::uint32_t id{}; };
    std::variant<std::monostate, Local, Upval, Global> where;
};

/**
 * @brief Function arity information
 */
struct Arity { std::uint16_t required{}; std::uint16_t optional{}; bool has_rest{}; };

struct Node; ///< fwd

/**
 * @brief Variable reference node
 *
 * Ownership: Node* pointers are owned by the ModuleSemantics arena.
 * The Ref<Node> wrapper can be used for additional type safety.
 */
struct Var   { Address addr; };

/**
 * @brief Literal values in the IR
 *
 * Numbers are stored directly without wrapper for efficiency.
 */
struct Literal { std::variant<std::monostate, bool, char32_t, std::string, int64_t, double> payload; };
struct Const { Literal value;
    Const(Literal v) : value(v) {}
    Const() = default;
};

/**
 * @brief Deep-copied datum (data-only subset) from parser for quotes
 *
 * Use shared_ptr so Nodes are copyable (tests/build systems may require copyable containers)
 */
struct Quote { std::shared_ptr<eta::reader::parser::SExpr> datum; };

/// Core IR node types
struct If    { Node* test; Node* conseq; Node* alt; };
struct Begin { std::vector<Node*> exprs; };
struct Set   { Address target; Node* value; };
struct DynamicWind { Node* before; Node* body; Node* after; };
struct Values { std::vector<Node*> exprs; };
struct CallWithValues { Node* producer; Node* consumer; };
struct CallCC { Node* consumer; };
struct Apply { Node* proc; std::vector<Node*> args; };

/**
 * @brief Exception raise: (raise 'tag value) or (raise value)
 *
 * tag_name: the symbol name to raise with (empty = no tag / catch-all raise)
 * value: the value to deliver to the handler
 */
struct Raise { std::string tag_name; Node* value; };

/**
 * @brief Exception guard: (catch 'tag body) or (catch body)
 *
 * tag_name: symbol to match (empty = catch any raise)
 * body: expression to protect; on raise the raised value becomes the result
 */
struct Guard { std::string tag_name; Node* body; };

/// Unification / logic variable IR nodes

struct MakeLogicVar {};

struct Unify { Node* a; Node* b; };

struct DerefLogicVar { Node* lvar; };

struct TrailMark {};

struct UnwindTrail { Node* mark; };

struct CopyTerm { Node* term; };

/**
 * @brief Lambda (function) node
 *
 * Contains parameter bindings, captured upvalues, and function body.
 */
struct Lambda {
    std::vector<BindingId> params;
    std::optional<BindingId> rest;
    std::vector<BindingId> locals;         ///< for frame layout
    std::vector<BindingId> upvals;         ///< ordered captures (internal IDs)
    std::vector<Address>   upval_sources;  ///< source addresses in parent scope
    Arity arity{};
    std::uint32_t stack_size{0};
    Node* body{};
};

/**
 * @brief Function call node
 */
struct Call { Node* callee; std::vector<Node*> args; };

/**
 * @brief Discriminated union of all IR node types
 *
 * This is the core intermediate representation for semantic analysis.
 * All Node* pointers within these types are owned by ModuleSemantics
 * and are valid for the lifetime of that module.
 *
 * Note: Let, LetRec, and Case are derived forms that are desugared by the
 * Expander before reaching the IR. They are not part of the core IR.
 */
using NodeData = std::variant<  Var,
                                Const,
                                Quote,
                                If,
                                Begin,
                                Set,
                                Lambda,
                                Call,
                                DynamicWind,
                                Values,
                                CallWithValues,
                                CallCC,
                                Apply,
                                Raise,
                                Guard,
                                MakeLogicVar,
                                Unify,
                                DerefLogicVar,
                                TrailMark,
                                UnwindTrail,
                                CopyTerm>;

struct Node {
    NodeData data;
    Span span;
    bool tail{false};

    template<typename T>
    Node(T&& d, Span s) : data(std::forward<T>(d)), span(s) {}
    Node() = default;
};

/// Re-export Ref for convenience
using NodeRef = Ref<Node>;

} ///< namespace eta::semantics::core
