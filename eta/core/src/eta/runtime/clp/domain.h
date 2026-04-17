/**
 * finite sets.  Pure C++ value types with no Eta/GC dependencies.
 *
 * Phase 4b: `FDDomain` is now a chunked bit-set rather than a sorted
 * `std::vector<int64_t>`.  Membership / cardinality / min / max all run
 * in O(words) instead of O(|values|), and the representation shares its
 * lifetime model with the Phase 4b unified `TrailEntry::Kind::Domain`
 * snapshot (the bitset chunks are copied straight into the trail entry,
 * no extra allocation per write).
 *
 * External invariants preserved:
 *   - `Domain` is `std::variant<ZDomain, FDDomain, RDomain>` (Phase 6.1
 *     added `RDomain` for clp(R) real-interval bounds).
 *   - `domain_intersect` still normalises across the Z/FD cross-kind
 *     promote the result to `RDomain` (the Z/FD integrality constraint
 *   - Empty intersection is reported as `empty() == true` on the result
 *     (callers use this to detect failure).
 *     does not participate (uncountable).
 */

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace eta::runtime::clp {

/// clp(Z): closed integer interval [lo, hi]

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

/**
 * clp(FD): chunked bit-set over a contiguous integer window
 * [base, base + 64*bits.size()).  Bit `b` of word `i` represents
 * membership of value `base + 64*i + b`.
 */

struct FDDomain {
    static constexpr int W = 64;

    int64_t               base  = 0;   ///< value at bit 0 of bits[0]
    std::vector<uint64_t> bits;        ///< chunked bit-set (little-endian within word)
    int64_t               count = 0;   ///< cached popcount of `bits`


    [[nodiscard]] bool    empty() const noexcept { return count == 0; }
    [[nodiscard]] int64_t size()  const noexcept { return count; }

    [[nodiscard]] bool contains(int64_t v) const noexcept {
        if (count == 0)   return false;
        if (v < base)     return false;
        const uint64_t off = static_cast<uint64_t>(v - base);
        const std::size_t w = static_cast<std::size_t>(off / W);
        if (w >= bits.size()) return false;
        return ((bits[w] >> (off % W)) & 1ull) != 0ull;
    }

    /// Smallest / largest member.  UB unless !empty().
    [[nodiscard]] int64_t min() const noexcept {
        for (std::size_t i = 0; i < bits.size(); ++i) {
            if (bits[i]) {
                return base + static_cast<int64_t>(i) * W
                            + std::countr_zero(bits[i]);
            }
        }
        return 0;
    }
    [[nodiscard]] int64_t max() const noexcept {
        for (std::size_t i = bits.size(); i-- > 0; ) {
            if (bits[i]) {
                return base + static_cast<int64_t>(i) * W
                            + (W - 1 - std::countl_zero(bits[i]));
            }
        }
        return 0;
    }

    /// Iterate every member in ascending order.  `f(int64_t)`.
    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < bits.size(); ++i) {
            uint64_t w = bits[i];
            const int64_t word_base = base + static_cast<int64_t>(i) * W;
            while (w) {
                const int b = std::countr_zero(w);
                w &= w - 1;
                f(word_base + b);
            }
        }
    }

    [[nodiscard]] std::vector<int64_t> to_vector() const {
        std::vector<int64_t> r;
        r.reserve(static_cast<std::size_t>(count));
        for_each([&](int64_t v) { r.push_back(v); });
        return r;
    }


    void recount() noexcept {
        int64_t c = 0;
        for (auto w : bits) c += std::popcount(w);
        count = c;
    }

    /**
     * Drop leading / trailing all-zero words; rebase accordingly.  Keeps
     * the bit-set as compact as possible after a narrowing.
     */
    void shrink_to_fit() {
        std::size_t lead = 0;
        while (lead < bits.size() && bits[lead] == 0) ++lead;
        if (lead == bits.size()) { bits.clear(); base = 0; count = 0; return; }
        std::size_t trail = bits.size();
        while (trail > lead && bits[trail - 1] == 0) --trail;
        if (lead != 0 || trail != bits.size()) {
            std::vector<uint64_t> nb(bits.begin() + lead, bits.begin() + trail);
            bits = std::move(nb);
            base += static_cast<int64_t>(lead) * W;
        }
    }


    /// Caller guarantees `vs` is sorted ascending and de-duplicated.
    static FDDomain from_sorted_unique(const std::vector<int64_t>& vs) {
        FDDomain d;
        if (vs.empty()) return d;
        d.base = vs.front();
        const int64_t span = vs.back() - vs.front();
        d.bits.assign(static_cast<std::size_t>(span / W + 1), 0ull);
        for (auto v : vs) {
            const uint64_t off = static_cast<uint64_t>(v - d.base);
            d.bits[off / W] |= (1ull << (off % W));
        }
        d.count = static_cast<int64_t>(vs.size());
        return d;
    }

    static FDDomain from_unsorted(std::vector<int64_t> vs) {
        std::sort(vs.begin(), vs.end());
        vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
        return from_sorted_unique(vs);
    }

    /// Materialise the closed interval [lo, hi].
    static FDDomain from_range(int64_t lo, int64_t hi) {
        FDDomain d;
        if (lo > hi) return d;
        d.base = lo;
        const int64_t total = hi - lo + 1;
        const std::size_t nwords =
            static_cast<std::size_t>((total + W - 1) / W);
        d.bits.assign(nwords, ~0ull);
        /// Mask off any high bits in the last word that overshoot `hi`.
        const int64_t used_in_last =
            total - static_cast<int64_t>(nwords - 1) * W;
        if (used_in_last < W) {
            d.bits.back() = (used_in_last <= 0)
                ? 0ull
                : ((used_in_last >= 64) ? ~0ull
                                        : ((1ull << used_in_last) - 1ull));
        }
        d.count = total;
        return d;
    }

    static FDDomain singleton(int64_t v) {
        FDDomain d;
        d.base  = v;
        d.bits.push_back(1ull);
        d.count = 1;
        return d;
    }


    [[nodiscard]] FDDomain intersect(const FDDomain& o) const {
        if (empty() || o.empty()) return {};
        const int64_t lo     = std::max(min(),  o.min());
        const int64_t hi     = std::min(max(),  o.max());
        if (lo > hi) return {};
        FDDomain r;
        r.base = lo;
        const int64_t span = hi - lo;
        r.bits.assign(static_cast<std::size_t>(span / W + 1), 0ull);
        /// Walk the smaller domain for fewer membership probes on the larger.
        const FDDomain& small = (count <= o.count) ? *this : o;
        const FDDomain& large = (count <= o.count) ? o     : *this;
        small.for_each([&](int64_t v) {
            if (v < lo || v > hi) return;
            if (!large.contains(v)) return;
            const uint64_t off = static_cast<uint64_t>(v - r.base);
            r.bits[off / W] |= (1ull << (off % W));
        });
        r.recount();
        r.shrink_to_fit();
        return r;
    }

    /// Intersect with the closed Z interval [lo, hi].
    [[nodiscard]] FDDomain intersect_z(int64_t lo, int64_t hi) const {
        if (empty() || lo > hi) return {};
        const int64_t my_min = min(), my_max = max();
        const int64_t nlo = std::max(my_min, lo);
        const int64_t nhi = std::min(my_max, hi);
        if (nlo > nhi) return {};
        FDDomain r;
        r.base = nlo;
        const int64_t span = nhi - nlo;
        r.bits.assign(static_cast<std::size_t>(span / W + 1), 0ull);
        for_each([&](int64_t v) {
            if (v < lo || v > hi) return;
            const uint64_t off = static_cast<uint64_t>(v - r.base);
            r.bits[off / W] |= (1ull << (off % W));
        });
        r.recount();
        r.shrink_to_fit();
        return r;
    }
};

/**
 * clp(R): real-valued interval [lo, hi] with per-bound open/closed flags.
 * just teaches the trail / unify / GC plumbing about a third domain kind
 * top without disturbing the Z/FD machinery.
 *
 * Conventions:
 *   - `empty()` iff lo > hi, or lo == hi with at least one open bound.
 *   - `contains(v)` honours the open/closed flags strictly.
 *   - The "size" of an R interval has no meaningful comparison with
 *     Z/FD cardinality; `size()` therefore returns `INT64_MAX` for any
 *     non-empty interval (callers using size for first-fail labelling
 *     fall through to a sentinel "treat as unbounded" branch).
 */

struct RDomain {
    double lo      = 0.0;
    double hi      = 0.0;
    bool   lo_open = false;
    bool   hi_open = false;

    [[nodiscard]] bool empty() const noexcept {
        if (lo > hi) return true;
        if (lo == hi && (lo_open || hi_open)) return true;
        return false;
    }

    [[nodiscard]] bool contains(double v) const noexcept {
        if (std::isnan(v)) return false;
        if (lo_open ? !(v > lo) : !(v >= lo)) return false;
        if (hi_open ? !(v < hi) : !(v <= hi)) return false;
        return true;
    }

    [[nodiscard]] int64_t size() const noexcept {
        return empty() ? 0 : std::numeric_limits<int64_t>::max();
    }

    /// min of upper bounds.  When the bounds tie, the open flag wins
    [[nodiscard]] RDomain intersect(const RDomain& o) const noexcept {
        RDomain r;
        if (lo > o.lo)        { r.lo = lo;   r.lo_open = lo_open; }
        else if (o.lo > lo)   { r.lo = o.lo; r.lo_open = o.lo_open; }
        else                  { r.lo = lo;   r.lo_open = lo_open || o.lo_open; }

        if (hi < o.hi)        { r.hi = hi;   r.hi_open = hi_open; }
        else if (o.hi < hi)   { r.hi = o.hi; r.hi_open = o.hi_open; }
        else                  { r.hi = hi;   r.hi_open = hi_open || o.hi_open; }
        return r;
    }

    [[nodiscard]] RDomain intersect_z(const ZDomain& z) const noexcept {
        if (z.empty()) { RDomain e{0,0,false,false}; e.lo = 1; e.hi = 0; return e; }
        const double zlo = static_cast<double>(z.lo);
        const double zhi = static_cast<double>(z.hi);
        RDomain r;
        if (lo > zlo)         { r.lo = lo;  r.lo_open = lo_open; }
        else if (zlo > lo)    { r.lo = zlo; r.lo_open = false; }
        else                  { r.lo = lo;  r.lo_open = lo_open; }

        if (hi < zhi)         { r.hi = hi;  r.hi_open = hi_open; }
        else if (zhi < hi)    { r.hi = zhi; r.hi_open = false; }
        else                  { r.hi = hi;  r.hi_open = hi_open; }
        return r;
    }
};

using Domain = std::variant<ZDomain, FDDomain, RDomain>;

[[nodiscard]] inline bool domain_empty(const Domain& d) {
    return std::visit([](const auto& x) { return x.empty(); }, d);
}

/**
 * Membership test for an integer value.  For `RDomain` this is the
 * natural mathematical containment of `v` in the (possibly half-open)
 * real interval; useful for forward-checking ground-integer unifies
 * against an R-domained var (the integer must lie in the interval).
 */
[[nodiscard]] inline bool domain_contains_int(const Domain& d, int64_t v) {
    if (auto* r = std::get_if<RDomain>(&d)) return r->contains(static_cast<double>(v));
    return std::visit([v](const auto& x) {
        if constexpr (std::is_same_v<std::decay_t<decltype(x)>, RDomain>)
            return x.contains(static_cast<double>(v));
        else
            return x.contains(v);
    }, d);
}

/**
 * when forward-checking a flonum unify against an R-domained var.
 * For Z/FD the value must equal an integer member exactly.
 */
[[nodiscard]] inline bool domain_contains_double(const Domain& d, double v) {
    if (auto* r = std::get_if<RDomain>(&d)) return r->contains(v);
    /// For Z/FD: only exact integers can be members.
    const double rv = std::round(v);
    if (rv != v) return false;
    if (rv < static_cast<double>(std::numeric_limits<int64_t>::min())) return false;
    if (rv > static_cast<double>(std::numeric_limits<int64_t>::max())) return false;
    const int64_t iv = static_cast<int64_t>(rv);
    if (auto* z  = std::get_if<ZDomain>(&d))  return z->contains(iv);
    if (auto* fd = std::get_if<FDDomain>(&d)) return fd->contains(iv);
    return false;
}

/**
 * Intersect two domains, normalising the cross-kind cases.
 * Result kind precedence:
 * Empty intersection yields a domain with `empty() == true`.
 */
[[nodiscard]] inline Domain domain_intersect(const Domain& a, const Domain& b) {
    if (auto* ra = std::get_if<RDomain>(&a)) {
        if (auto* rb = std::get_if<RDomain>(&b))  return ra->intersect(*rb);
        if (auto* zb = std::get_if<ZDomain>(&b))  return ra->intersect_z(*zb);
        const auto& fb = std::get<FDDomain>(b);
        if (fb.empty()) { RDomain r{0,0,false,false}; r.lo = 1; r.hi = 0; return r; }
        return ra->intersect_z(ZDomain{fb.min(), fb.max()});
    }
    if (auto* rb = std::get_if<RDomain>(&b)) {
        if (auto* za = std::get_if<ZDomain>(&a))  return rb->intersect_z(*za);
        const auto& fa = std::get<FDDomain>(a);
        if (fa.empty()) { RDomain r{0,0,false,false}; r.lo = 1; r.hi = 0; return r; }
        return rb->intersect_z(ZDomain{fa.min(), fa.max()});
    }
    if (auto* za = std::get_if<ZDomain>(&a)) {
        if (auto* zb = std::get_if<ZDomain>(&b)) return za->intersect(*zb);
        const auto& fb = std::get<FDDomain>(b);
        return fb.intersect_z(za->lo, za->hi);
    }
    const auto& fa = std::get<FDDomain>(a);
    if (auto* zb = std::get_if<ZDomain>(&b)) return fa.intersect_z(zb->lo, zb->hi);
    return fa.intersect(std::get<FDDomain>(b));
}

} ///< namespace eta::runtime::clp

