#include "eta/runtime/value_formatter.h"

namespace eta::runtime {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;

std::string format_value(LispVal v, FormatMode mode, Heap& heap, InternTable& intern_table) {
    if (v == nanbox::Nil) return "()";
    if (v == nanbox::True) return "#t";
    if (v == nanbox::False) return "#f";

    if (!ops::is_boxed(v)) {
        // Raw double (unboxed flonum)
        std::ostringstream oss;
        double d = std::bit_cast<double>(v);
        oss << d;
        std::string s = oss.str();
        // Ensure there's a decimal point for write mode
        if (mode == FormatMode::Write && s.find('.') == std::string::npos && s.find('e') == std::string::npos
            && s.find('E') == std::string::npos && s != "inf" && s != "-inf" && s != "nan" && s != "-nan") {
            s += ".0";
        }
        return s;
    }

    Tag t = ops::tag(v);

    if (t == Tag::Fixnum) {
        auto val = ops::decode<int64_t>(v);
        if (val) return std::to_string(*val);
        return "#<fixnum?>";
    }

    if (t == Tag::Char) {
        auto val = ops::decode<char32_t>(v);
        if (!val) return "#<char?>";
        char32_t c = *val;

        if (mode == FormatMode::Display) {
            // Display: raw UTF-8 character
            if (c < 0x80) {
                return std::string(1, static_cast<char>(c));
            }
            return utf8::encode(c);
        }

        // Write mode: #\<name> or #\<char>
        // Using if-else instead of switch to avoid MSVC optimizer issues
        if (c == U' ')    return "#\\space";
        if (c == U'\n')   return "#\\newline";
        if (c == U'\r')   return "#\\return";
        if (c == U'\t')   return "#\\tab";
        if (c == U'\0')   return "#\\null";
        if (c == 0x1B)    return "#\\escape";
        if (c == 0x7F)    return "#\\delete";
        if (c == U'\a')   return "#\\alarm";
        if (c == U'\b')   return "#\\backspace";

        if (c >= 0x21 && c <= 0x7E) {
            // Printable ASCII - use array initialization to avoid potential issues
            char buf[4] = {'#', '\\', static_cast<char>(c), '\0'};
            return std::string(buf);
        }
        // Non-printable or non-ASCII: #\xHEX
        std::ostringstream oss;
        oss << "#\\x" << std::hex << std::uppercase << static_cast<uint32_t>(c);
        return oss.str();
    }

    if (t == Tag::String) {
        auto sv = StringView::try_from(v, intern_table);
        if (!sv) return "#<string?>";
        std::string_view raw = sv->view();

        if (mode == FormatMode::Display) {
            return std::string(raw);
        }

        // Write mode: quote and escape
        std::string out = "\"";
        for (char ch : raw) {
            switch (ch) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\a': out += "\\a"; break;
                case '\b': out += "\\b"; break;
                default:   out += ch; break;
            }
        }
        out += '"';
        return out;
    }

    if (t == Tag::Symbol) {
        auto sv = intern_table.get_string(ops::payload(v));
        if (sv) return std::string(*sv);
        return "#<symbol?>";
    }

    if (t == Tag::TapeRef) {
        return "#<tape-ref:" + std::to_string(ops::payload(v)) + ">";
    }

    if (t == Tag::HeapObject) {
        auto id = ops::payload(v);

        // Numeric (heap-allocated fixnum or flonum)
        auto n = classify_numeric(v, heap);
        if (n.is_fixnum()) {
            return std::to_string(n.int_val);
        }
        if (n.is_flonum()) {
            std::ostringstream oss;
            oss << n.float_val;
            return oss.str();
        }

        // Cons (pair / list)
        if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
            std::string out = "(";
            out += format_value(cons->car, mode, heap, intern_table);
            LispVal rest = cons->cdr;
            // Walk the cdr chain
            while (rest != nanbox::Nil) {
                if (ops::is_boxed(rest) && ops::tag(rest) == Tag::HeapObject) {
                    if (auto* next = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(rest))) {
                        out += " ";
                        out += format_value(next->car, mode, heap, intern_table);
                        rest = next->cdr;
                        continue;
                    }
                }
                // Dotted pair
                out += " . ";
                out += format_value(rest, mode, heap, intern_table);
                break;
            }
            out += ")";
            return out;
        }

        // Vector
        if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
            std::string out = "#(";
            for (size_t i = 0; i < vec->elements.size(); ++i) {
                if (i > 0) out += " ";
                out += format_value(vec->elements[i], mode, heap, intern_table);
            }
            out += ")";
            return out;
        }

        // ByteVector
        if (auto* bv = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(id)) {
            std::string out = "#u8(";
            for (size_t i = 0; i < bv->data.size(); ++i) {
                if (i > 0) out += " ";
                out += std::to_string(bv->data[i]);
            }
            out += ")";
            return out;
        }

        // Closure
        if (heap.try_get_as<ObjectKind::Closure, types::Closure>(id)) {
            return "#<closure>";
        }

        // Primitive
        if (heap.try_get_as<ObjectKind::Primitive, types::Primitive>(id)) {
            return "#<primitive>";
        }

        // Continuation
        if (heap.try_get_as<ObjectKind::Continuation, types::Continuation>(id)) {
            return "#<continuation>";
        }

        // Port
        if (heap.try_get_as<ObjectKind::Port, types::PortObject>(id)) {
            return "#<port>";
        }

        // Logic variable
        if (auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
            if (lv->binding.has_value()) {
                return format_value(*lv->binding, mode, heap, intern_table);
            }
            if (!lv->name.empty()) return "_" + lv->name + "G" + std::to_string(id);
            return "_G" + std::to_string(id);
        }

        // Compound term: f(arg1, arg2, ...)
        if (auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
            std::string out = format_value(ct->functor, mode, heap, intern_table);
            out += "(";
            for (std::size_t i = 0; i < ct->args.size(); ++i) {
                if (i) out += ", ";
                out += format_value(ct->args[i], mode, heap, intern_table);
            }
            out += ")";
            return out;
        }

        // AD Tape
        if (heap.try_get_as<ObjectKind::Tape, types::Tape>(id)) {
            return "#<tape>";
        }

        // Torch tensor (opaque — libtorch manages storage)
        if (heap.try_get_as<ObjectKind::Tensor, void>(id)) {
            return "#<tensor>";
        }

        // NN module (opaque)
        if (heap.try_get_as<ObjectKind::NNModule, void>(id)) {
            return "#<nn-module>";
        }

        // Optimizer (opaque)
        if (heap.try_get_as<ObjectKind::Optimizer, void>(id)) {
            return "#<optimizer>";
        }

        // Fact table
        if (auto* ft = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(id)) {
            return "#<fact-table " + std::to_string(ft->col_names.size())
                   + "cols×" + std::to_string(ft->row_count) + "rows>";
        }

        return "#<object>";
    }

    return "#<unknown>";
}

} // namespace eta::runtime

