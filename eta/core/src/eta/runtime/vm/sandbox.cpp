#include "sandbox.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/runtime/numeric_ops.h"
#include "eta/runtime/types/cons.h"
#include "eta/runtime/types/hash_map.h"
#include "eta/runtime/types/hash_set.h"

namespace eta::runtime::vm {

namespace {

using namespace eta::runtime::nanbox;
using eta::reader::lexer::Lexer;
using eta::reader::parser::Parser;
using SExpr    = eta::reader::parser::SExpr;
using SExprPtr = eta::reader::parser::SExprPtr;
using List     = eta::reader::parser::List;
using Symbol   = eta::reader::parser::Symbol;
using Number   = eta::reader::parser::Number;
using StringL  = eta::reader::parser::String;
using Bool     = eta::reader::parser::Bool;
using Char     = eta::reader::parser::Char;
using NilL     = eta::reader::parser::Nil;
using ReaderForm = eta::reader::parser::ReaderForm;
using QuoteKind  = eta::reader::parser::QuoteKind;

/// Errors that propagate out of the recursive evaluator.
struct EvalError {
    std::string text;
    bool        violation{false};
};

/**
 * Lexical environment: a stack of frames. Most recent (deepest let) wins.
 * The sandbox creates new frames on `let` only - host VM locals are never
 * mutated.
 */
struct Env {
    std::vector<std::unordered_map<std::string, LispVal>> frames;

    bool find(const std::string& name, LispVal& out) const {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            auto fit = it->find(name);
            if (fit != it->end()) { out = fit->second; return true; }
        }
        return false;
    }
};

class Evaluator {
public:
    Evaluator(memory::heap::Heap& heap,
              memory::intern::InternTable& intern,
              const Sandbox::Lookup& lookup)
        : heap_(heap), intern_(intern), lookup_(lookup) {}

    std::expected<LispVal, EvalError> eval(const SExpr& e, Env& env) {
        if (e.is<NilL>())   return Nil;
        if (auto* b = e.as<Bool>())   return b->value ? True : False;
        if (auto* c = e.as<Char>())   {
            auto enc = ops::encode<char32_t>(c->value);
            if (!enc) return err("invalid character literal");
            return *enc;
        }
        if (auto* n = e.as<Number>()) return number_to_value(n->value);
        if (auto* s = e.as<StringL>()) return string_literal(s->value);
        if (auto* sym = e.as<Symbol>()) return resolve_symbol(sym->name);
        if (auto* rf = e.as<ReaderForm>()) {
            if (rf->kind == QuoteKind::Quote) return eval_quoted(*rf->expr);
            return violation("only 'quote reader-forms are allowed in sandbox");
        }
        if (auto* lst = e.as<List>()) return eval_list(*lst, env);
        return violation("unsupported s-expression in sandbox");
    }

private:
    memory::heap::Heap&             heap_;
    memory::intern::InternTable&    intern_;
    const Sandbox::Lookup&          lookup_;

    static std::expected<LispVal, EvalError> err(std::string msg) {
        return std::unexpected(EvalError{std::move(msg), false});
    }
    static std::expected<LispVal, EvalError> violation(std::string msg) {
        return std::unexpected(EvalError{std::move(msg), true});
    }

    static bool is_truthy(LispVal v) noexcept { return v != False; }

    std::expected<LispVal, EvalError> number_to_value(const eta::Number& n) {
        if (auto* i = std::get_if<int64_t>(&n)) {
            auto enc = ops::encode<int64_t>(*i);
            if (!enc) return err("integer out of fixnum range");
            return *enc;
        }
        if (auto* d = std::get_if<double>(&n)) {
            auto enc = ops::encode<double>(*d);
            if (!enc) return err("invalid floating-point literal");
            return *enc;
        }
        return err("unsupported numeric literal");
    }

    std::expected<LispVal, EvalError> string_literal(const std::string& s) {
        auto sid = intern_.intern(s);
        if (!sid) return err("failed to intern string literal");
        return ops::box(Tag::String, *sid);
    }

    std::expected<LispVal, EvalError> resolve_symbol(const std::string& name) {
        LispVal v = Nil;
        if (lookup_ && lookup_(name, v)) return v;
        return err("identifier not found in paused frame: " + name);
    }

    std::expected<LispVal, EvalError> eval_quoted(const SExpr& e) {
        if (auto* sym = e.as<Symbol>()) {
            auto sid = intern_.intern(sym->name);
            if (!sid) return err("failed to intern symbol literal");
            return ops::box(Tag::Symbol, *sid);
        }
        if (e.is<NilL>()) return Nil;
        if (auto* lst = e.as<List>()) {
            if (lst->elems.empty() && !lst->dotted) return Nil;
            return violation("only quoted symbols and '() are allowed in sandbox");
        }
        if (auto* b = e.as<Bool>())   return b->value ? True : False;
        if (auto* n = e.as<Number>()) return number_to_value(n->value);
        if (auto* s = e.as<StringL>()) return string_literal(s->value);
        return violation("unsupported quoted form in sandbox");
    }

    std::expected<LispVal, EvalError> eval_list(const List& lst, Env& env) {
        if (lst.dotted) return violation("dotted call form not supported");
        if (lst.elems.empty()) return Nil;
        const auto* head_sym = lst.elems.front()->as<Symbol>();
        if (!head_sym) {
            return violation("call head must be a symbol; computed application not allowed");
        }
        const std::string& op = head_sym->name;

        /// Special forms first
        if (op == "if")     return form_if(lst, env);
        if (op == "and")    return form_and(lst, env);
        if (op == "or")     return form_or(lst, env);
        if (op == "let")    return form_let(lst, env);
        if (op == "begin" || op == "progn") return form_begin(lst, env);
        if (op == "quote") {
            if (lst.elems.size() != 2) return err("quote expects exactly 1 argument");
            return eval_quoted(*lst.elems[1]);
        }

        /// Evaluate operands left to right
        std::vector<LispVal> args;
        args.reserve(lst.elems.size() - 1);
        for (std::size_t i = 1; i < lst.elems.size(); ++i) {
            auto v = eval(*lst.elems[i], env);
            if (!v) return std::unexpected(v.error());
            args.push_back(*v);
        }
        return apply_builtin(op, args);
    }

    /// ------- special forms -------

    std::expected<LispVal, EvalError> form_if(const List& lst, Env& env) {
        if (lst.elems.size() < 3 || lst.elems.size() > 4)
            return err("if expects 2 or 3 operands");
        auto c = eval(*lst.elems[1], env);
        if (!c) return c;
        if (is_truthy(*c)) return eval(*lst.elems[2], env);
        if (lst.elems.size() == 4) return eval(*lst.elems[3], env);
        return Nil;
    }

    std::expected<LispVal, EvalError> form_and(const List& lst, Env& env) {
        LispVal last = True;
        for (std::size_t i = 1; i < lst.elems.size(); ++i) {
            auto v = eval(*lst.elems[i], env);
            if (!v) return v;
            if (!is_truthy(*v)) return False;
            last = *v;
        }
        return last;
    }

    std::expected<LispVal, EvalError> form_or(const List& lst, Env& env) {
        for (std::size_t i = 1; i < lst.elems.size(); ++i) {
            auto v = eval(*lst.elems[i], env);
            if (!v) return v;
            if (is_truthy(*v)) return *v;
        }
        return False;
    }

    std::expected<LispVal, EvalError> form_let(const List& lst, Env& env) {
        if (lst.elems.size() < 3) return err("let expects bindings and body");
        const auto* bindings = lst.elems[1]->as<List>();
        if (!bindings) return err("let bindings must be a list");

        std::unordered_map<std::string, LispVal> frame;
        for (const auto& b : bindings->elems) {
            const auto* pair = b->as<List>();
            if (!pair || pair->elems.size() != 2)
                return err("let binding must be (name expr)");
            const auto* name = pair->elems[0]->as<Symbol>();
            if (!name) return err("let binding name must be a symbol");
            auto v = eval(*pair->elems[1], env);
            if (!v) return v;
            frame[name->name] = *v;
        }
        env.frames.push_back(std::move(frame));
        std::expected<LispVal, EvalError> last = Nil;
        for (std::size_t i = 2; i < lst.elems.size(); ++i) {
            last = eval(*lst.elems[i], env);
            if (!last) break;
        }
        env.frames.pop_back();
        return last;
    }

    std::expected<LispVal, EvalError> form_begin(const List& lst, Env& env) {
        std::expected<LispVal, EvalError> last = Nil;
        for (std::size_t i = 1; i < lst.elems.size(); ++i) {
            last = eval(*lst.elems[i], env);
            if (!last) return last;
        }
        return last;
    }

    /// ------- builtins (read-only) -------

    std::expected<LispVal, EvalError> apply_builtin(
        const std::string& op,
        const std::vector<LispVal>& args)
    {
        /// Equality / boolean
        if (op == "not") {
            if (args.size() != 1) return err("not expects 1 argument");
            return is_truthy(args[0]) ? False : True;
        }
        if (op == "eq?") {
            if (args.size() != 2) return err("eq? expects 2 arguments");
            return args[0] == args[1] ? True : False;
        }
        if (op == "equal?") {
            if (args.size() != 2) return err("equal? expects 2 arguments");
            return values_equal(args[0], args[1]) ? True : False;
        }

        /// Predicates
        if (op == "null?") {
            if (args.size() != 1) return err("null? expects 1 argument");
            return args[0] == Nil ? True : False;
        }
        if (op == "boolean?") {
            if (args.size() != 1) return err("boolean? expects 1 argument");
            return (args[0] == True || args[0] == False) ? True : False;
        }
        if (op == "pair?") {
            if (args.size() != 1) return err("pair? expects 1 argument");
            return is_cons(args[0]) ? True : False;
        }
        if (op == "number?" || op == "integer?") {
            if (args.size() != 1) return err(op + " expects 1 argument");
            const bool is_int = ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::Fixnum;
            const bool is_dbl = !ops::is_boxed(args[0]);
            if (op == "integer?") return is_int ? True : False;
            return (is_int || is_dbl) ? True : False;
        }
        if (op == "string?") {
            if (args.size() != 1) return err("string? expects 1 argument");
            return (ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::String) ? True : False;
        }
        if (op == "symbol?") {
            if (args.size() != 1) return err("symbol? expects 1 argument");
            return (ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::Symbol) ? True : False;
        }
        if (op == "char?") {
            if (args.size() != 1) return err("char? expects 1 argument");
            return (ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::Char) ? True : False;
        }

        /// Arithmetic
        if (op == "+" || op == "-" || op == "*" || op == "/") {
            return arith(op, args);
        }
        if (op == "mod" || op == "modulo" || op == "remainder") {
            if (args.size() != 2) return err(op + " expects 2 arguments");
            int64_t a, b;
            if (!as_int(args[0], a) || !as_int(args[1], b))
                return err(op + ": integer arguments required");
            if (b == 0) return err(op + ": division by zero");
            int64_t r = a % b;
            if ((op == "mod" || op == "modulo") && (r != 0) && ((r < 0) != (b < 0))) {
                r += b;
            }
            auto enc = ops::encode<int64_t>(r);
            if (!enc) return err(op + ": result out of fixnum range");
            return *enc;
        }
        if (op == "=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            if (args.size() != 2) return err(op + " expects 2 arguments");
            NumericValue a{};
            NumericValue b{};
            if (!numeric_ops::classify_immediate(args[0], a) || !numeric_ops::classify_immediate(args[1], b))
                return err(op + ": numeric arguments required");
            using numeric_ops::CmpOp;
            const CmpOp cmp = op == "="  ? CmpOp::NumEq
                            : op == "<"  ? CmpOp::Lt
                            : op == ">"  ? CmpOp::Gt
                            : op == "<=" ? CmpOp::Le
                                         : CmpOp::Ge;
            return numeric_ops::compare(cmp, a, b) ? True : False;
        }

        /// List access (read-only - no allocation)
        if (op == "car" || op == "cdr") {
            if (args.size() != 1) return err(op + " expects 1 argument");
            const auto* c = try_get_cons(args[0]);
            if (!c) return err(op + ": argument is not a pair");
            return op == "car" ? c->car : c->cdr;
        }
        if (op == "length") {
            if (args.size() != 1) return err("length expects 1 argument");
            int64_t n = 0;
            LispVal cur = args[0];
            while (cur != Nil) {
                const auto* c = try_get_cons(cur);
                if (!c) return err("length: argument is not a proper list");
                ++n;
                cur = c->cdr;
            }
            auto enc = ops::encode<int64_t>(n);
            if (!enc) return err("length: result out of fixnum range");
            return *enc;
        }

        /// String inspection (no allocation)
        if (op == "string-length") {
            if (args.size() != 1) return err("string-length expects 1 argument");
            if (!(ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::String))
                return err("string-length: argument is not a string");
            auto sv = intern_.get_string(ops::payload(args[0]));
            if (!sv) return err("string-length: missing intern entry");
            auto enc = ops::encode<int64_t>(static_cast<int64_t>(sv->size()));
            if (!enc) return err("string-length: result out of fixnum range");
            return *enc;
        }
        if (op == "string=?") {
            if (args.size() != 2) return err("string=? expects 2 arguments");
            for (auto v : args) {
                if (!(ops::is_boxed(v) && ops::tag(v) == Tag::String))
                    return err("string=?: arguments must be strings");
            }
            return args[0] == args[1] ? True : False;
        }

        return violation("operator not allowed in sandbox: " + op);
    }

    /// ------- helpers -------

    bool as_int(LispVal v, int64_t& out) const noexcept {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::Fixnum) return false;
        out = ops::sign_extend_fixnum(ops::payload(v));
        return true;
    }

    bool is_cons(LispVal v) const noexcept {
        return ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject &&
               heap_.try_get_as<memory::heap::ObjectKind::Cons, types::Cons>(ops::payload(v)) != nullptr;
    }
    const types::Cons* try_get_cons(LispVal v) const noexcept {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
        return heap_.try_get_as<memory::heap::ObjectKind::Cons, types::Cons>(ops::payload(v));
    }
    const types::HashMap* try_get_hash_map(LispVal v) const noexcept {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
        return heap_.try_get_as<memory::heap::ObjectKind::HashMap, types::HashMap>(ops::payload(v));
    }
    const types::HashSet* try_get_hash_set(LispVal v) const noexcept {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
        return heap_.try_get_as<memory::heap::ObjectKind::HashSet, types::HashSet>(ops::payload(v));
    }

    bool values_equal(LispVal a, LispVal b) const noexcept {
        if (a == b) return true;

        const auto* ca = try_get_cons(a);
        const auto* cb = try_get_cons(b);
        if (ca && cb) return values_equal(ca->car, cb->car) && values_equal(ca->cdr, cb->cdr);

        const auto* hma = try_get_hash_map(a);
        const auto* hmb = try_get_hash_map(b);
        if (hma && hmb) {
            if (hma->size != hmb->size) return false;
            for (std::size_t i = 0; i < hma->state.size(); ++i) {
                if (hma->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                bool found = false;
                for (std::size_t j = 0; j < hmb->state.size(); ++j) {
                    if (hmb->state[j] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                    if (!values_equal(hma->keys[i], hmb->keys[j])) continue;
                    if (!values_equal(hma->values[i], hmb->values[j])) return false;
                    found = true;
                    break;
                }
                if (!found) return false;
            }
            return true;
        }

        const auto* hsa = try_get_hash_set(a);
        const auto* hsb = try_get_hash_set(b);
        if (hsa && hsb) {
            if (hsa->table.size != hsb->table.size) return false;
            for (std::size_t i = 0; i < hsa->table.state.size(); ++i) {
                if (hsa->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                bool found = false;
                for (std::size_t j = 0; j < hsb->table.state.size(); ++j) {
                    if (hsb->table.state[j] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                    if (values_equal(hsa->table.keys[i], hsb->table.keys[j])) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }

        return false;
    }

    std::expected<LispVal, EvalError> arith(
        const std::string& op,
        const std::vector<LispVal>& args)
    {
        /// Same semantics as the runtime (numeric_ops), restricted to immediates so nothing allocates
        using numeric_ops::ArithOp;
        const ArithOp aop = op == "+" ? ArithOp::Add
                          : op == "-" ? ArithOp::Sub
                          : op == "*" ? ArithOp::Mul
                                      : ArithOp::Div;

        std::vector<NumericValue> nums(args.size());
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (!numeric_ops::classify_immediate(args[i], nums[i])) {
                return err(op + ": numeric arguments required");
            }
        }

        const NumericValue zero{NumType::Fixnum, 0, 0.0};
        const NumericValue one{NumType::Fixnum, 1, 0.0};
        const NumericValue minus_one{NumType::Fixnum, -1, 0.0};

        NumericValue acc{};
        if (nums.empty()) {
            if (aop == ArithOp::Add) acc = zero;
            else if (aop == ArithOp::Mul) acc = one;
            else return err(op + " requires at least 1 argument");
        } else if (nums.size() == 1) {
            /// (- x) is -1 * x so that (- 0.0) is -0.0; (/ x) is (/ 1 x)
            acc = nums[0];
            if (aop == ArithOp::Sub || aop == ArithOp::Div) {
                auto r = aop == ArithOp::Sub ? numeric_ops::arith(ArithOp::Mul, minus_one, acc)
                                             : numeric_ops::arith(ArithOp::Div, one, acc);
                if (!r) return err(op + ": division by zero");
                acc = *r;
            }
        } else {
            acc = nums[0];
            for (std::size_t i = 1; i < nums.size(); ++i) {
                auto r = numeric_ops::arith(aop, acc, nums[i]);
                if (!r) return err(op + ": division by zero");
                acc = *r;
            }
        }

        if (acc.is_flonum()) {
            auto enc = ops::encode<double>(acc.float_val);
            if (!enc) return err(op + ": invalid floating-point result");
            return *enc;
        }
        auto enc = ops::encode<int64_t>(acc.int_val);
        if (!enc) return err(op + ": result out of fixnum range");
        return *enc;
    }
};

} ///< anonymous namespace

Sandbox::Sandbox(memory::heap::Heap& heap,
                 memory::intern::InternTable& intern_table,
                 Lookup lookup)
    : heap_(heap), intern_table_(intern_table), lookup_(std::move(lookup)) {}

SandboxResult Sandbox::eval(const std::string& expr_text) {
    SandboxResult out;

    /**
     * Tokenize the expression with file_id=0 so spans don't collide with any
     * real source file. The sandbox parser is never recovered into the host
     * driver's diagnostics machinery.
     */
    Lexer lexer(0, expr_text);
    Parser parser(lexer);
    auto datum = parser.parse_datum();
    if (!datum) {
        out.error = "sandbox parse error";
        return out;
    }
    if (!*datum) {
        out.error = "sandbox: empty expression";
        return out;
    }

    Evaluator ev(heap_, intern_table_, lookup_);
    Env env;
    env.frames.emplace_back();
    auto r = ev.eval(**datum, env);
    if (!r) {
        out.error     = std::move(r.error().text);
        out.violation = r.error().violation;
        return out;
    }
    out.value = *r;
    return out;
}

} ///< namespace eta::runtime::vm

