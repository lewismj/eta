// clp/domain.h — CLP domain types: clp(Z) intervals and clp(FD) finite sets.
// These are pure C++ value types with no Eta/GC dependencies.

#pragma once

#include <algorithm>
#include <cstdint>
#include <variant>
#include <vector>

namespace eta::runtime::clp {

// clp(Z): closed integer interval [lo, hi]

struct ZDomain {
    int64_t lo;
    int64_t hi;

    [[nodiscard]] bool empty()             const noexcept { return lo > hi; }
    [[nodiscard]] bool contains(int64_t v) const noexcept { return v >= lo && v <= hi; }

    [[nodiscard]] ZDomain intersect(const ZDomain& o) const noexcept {
        return { std::max(lo, o.lo), std::min(hi, o.hi) };
    }
    [[nodiscard]] int64_t size() const noexcept {
        return empty() ? 0 : (hi - lo + 1);
    }
};

// clp(FD): sorted, deduplicated finite set of integers

struct FDDomain {
    std::vector<int64_t> values;  // always sorted and deduplicated

    [[nodiscard]] bool empty() const noexcept { return values.empty(); }
    [[nodiscard]] bool contains(int64_t v) const noexcept {
        return std::binary_search(values.begin(), values.end(), v);
    }

    [[nodiscard]] FDDomain intersect(const FDDomain& o) const {
        FDDomain r;
        std::set_intersection(values.begin(), values.end(),
                              o.values.begin(), o.values.end(),
                              std::back_inserter(r.values));
        return r;
    }

    // Intersect a FD domain with a Z interval (clp:= between FD and Z vars).
    [[nodiscard]] FDDomain intersect_z(int64_t lo, int64_t hi) const {
        FDDomain r;
        for (auto v : values)
            if (v >= lo && v <= hi) r.values.push_back(v);
        return r;
    }
};

// Tagged union over the two domain kinds

using Domain = std::variant<ZDomain, FDDomain>;

[[nodiscard]] inline bool domain_empty(const Domain& d) {
    return std::visit([](const auto& x) { return x.empty(); }, d);
}

[[nodiscard]] inline bool domain_contains_int(const Domain& d, int64_t v) {
    return std::visit([v](const auto& x) { return x.contains(v); }, d);
}

} // namespace eta::runtime::clp

