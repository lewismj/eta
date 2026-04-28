#pragma once

#include <functional>
#include <expected>
#include <initializer_list>
#include <span>
#include <vector>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::error;

    /**
     * @brief Lightweight primitive-argument view.
     *
     * Supports zero-copy calls from VM stack spans while preserving existing
     * call sites that pass vectors or brace-initializer arguments.
     */
    class PrimitiveArgs {
    public:
        PrimitiveArgs(std::span<const LispVal> view) : view_(view) {}
        PrimitiveArgs(const std::vector<LispVal>& vec) : view_(vec) {}
        PrimitiveArgs(std::initializer_list<LispVal> init) : owned_(init), view_(owned_) {}
        PrimitiveArgs(std::vector<LispVal>&&) = delete;

        [[nodiscard]] std::size_t size() const noexcept { return view_.size(); }
        [[nodiscard]] bool empty() const noexcept { return view_.empty(); }
        [[nodiscard]] const LispVal* data() const noexcept { return view_.data(); }
        [[nodiscard]] const LispVal& operator[](std::size_t idx) const noexcept { return view_[idx]; }
        [[nodiscard]] auto begin() const noexcept { return view_.begin(); }
        [[nodiscard]] auto end() const noexcept { return view_.end(); }
        [[nodiscard]] operator std::span<const LispVal>() const noexcept { return view_; }

    private:
        std::vector<LispVal> owned_{};
        std::span<const LispVal> view_{};
    };

    using PrimitiveFunc = std::function<std::expected<LispVal, RuntimeError>(PrimitiveArgs)>;

    struct Primitive {
        PrimitiveFunc func {};
        uint32_t arity {};
        bool has_rest {};
        std::vector<LispVal> gc_roots {};  ///< LispVals captured by func that GC must trace
    };

}
