#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "eta/reader/parser.h"

namespace eta::semantics::core {

using eta::reader::parser::Span;
using eta::reader::parser::SExprPtr;

struct BindingId {
    std::uint32_t id{};
    auto operator<=>(const BindingId&) const = default;
};

// Address where a variable is read/written from after closure conversion
struct Address {
    struct Local  { std::uint16_t slot{}; };
    struct Upval  { std::uint16_t depth{}; std::uint16_t slot{}; };
    struct Global { std::uint32_t id{}; };
    std::variant<Local, Upval, Global> where;
};

struct Arity { std::uint16_t required{}; std::uint16_t optional{}; bool has_rest{}; };

struct Node; // fwd

struct Var   { Address addr; Span span; };

struct LiteralNumber { enum { Fixnum, Flonum } kind; std::string text; std::uint8_t radix{10}; };
struct Literal { std::variant<std::monostate, bool, char32_t, std::string, LiteralNumber> payload; };
struct Const { Literal value; Span span;
    Const(Literal v, Span s) : value(v), span(s) {}
    Const() = default;
};

// Deep-copied datum (data-only subset) from parser for quotes
// Use shared_ptr so Nodes are copyable (tests/build systems may require copyable containers)
struct Quote { std::shared_ptr<eta::reader::parser::SExpr> datum; Span span; };

struct If    { Node* test; Node* conseq; Node* alt; bool tail{false}; Span span; };
struct Begin { std::vector<Node*> exprs; bool tail{false}; Span span; };
struct Let   { struct Bind { BindingId b; Node* init; }; std::vector<Bind> binds; Node* body; bool tail{false}; Span span; };
struct LetRec{ std::vector<BindingId> binds; Node* body; bool tail{false}; Span span; };
struct Set   { Address target; Node* value; Span span; };
struct DynamicWind { Node* before; Node* body; Node* after; Span span; };
struct Case  { Node* key; struct Clause { std::vector<std::shared_ptr<eta::reader::parser::SExpr>> datums; Node* body; bool is_else; }; std::vector<Clause> clauses; Span span; };
struct Values { std::vector<Node*> exprs; Span span; };
struct CallWithValues { Node* producer; Node* consumer; bool tail{false}; Span span; };
struct CallCC { Node* consumer; bool tail{false}; Span span; };

struct Lambda {
    std::vector<BindingId> params;
    std::optional<BindingId> rest;
    std::vector<BindingId> locals;  // for frame layout
    std::vector<BindingId> upvals;  // ordered captures
    Arity arity{};
    Node* body{};
    Span span{};
};

struct Call { Node* callee; std::vector<Node*> args; bool tail{false}; Span span; };

using NodeBase = std::variant<Var, Const, Quote, If, Begin, Let, LetRec, Set, Lambda, Call, DynamicWind, Case, Values, CallWithValues, CallCC>;
struct Node : NodeBase { using NodeBase::NodeBase; };

} // namespace eta::semantics::core
