// clp/alldiff_regin.h — Régin's value-graph matching algorithm for
// domain-consistent all_different/1.
//
// Given a list of variables with FD domains, this computes the set of
// (var,value) edges that can be safely removed while preserving *every*
// solution of the all-different constraint.  Pruning is strictly stronger
// than pairwise forward-checking: for instance, on X ∈ {1,2}, Y ∈ {1,2},
// Z ∈ {1,2,3} with all_different(X,Y,Z), Régin prunes 1 and 2 from Z's
// domain (leaving Z = 3), which a pairwise loop never discovers.
//
// Algorithm (Régin 1994):
//   1. Build bipartite graph G = (Xs ∪ Vs, E) with E = {(x,v) : v ∈ D(x)}.
//   2. Find a maximum matching M on G.  If |M| < |Xs| → infeasible.
//   3. Orient edges of G into a directed graph D:
//        matched edge (x,v) ∈ M  →  x → v
//        unmatched edge (x,v)    →  v → x
//   4. Let F ⊆ Vs be the set of "free" values (not matched by M).
//   5. Compute SCCs of D via Tarjan.
//   6. Compute the set R ⊆ (Xs ∪ Vs) of nodes reachable from some v ∈ F
//      using D's edges (forward BFS).
//   7. For each edge (x,v) ∈ E:
//        - Keep if (x,v) ∈ M.
//        - Keep if SCC(x) == SCC(v).
//        - Keep if v ∈ R.
//        - Otherwise REMOVE v from D(x).
//
// Returns `true` on success (possibly with pruned domains), `false` on
// detected infeasibility.  Domain writes are performed via the caller-
// supplied `narrow_fn` callback which must be trail-aware.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace eta::runtime::clp {

struct AlldiffVar {
    int64_t              ground_val = 0;  // meaningful iff `is_ground`
    std::vector<int64_t> domain;          // sorted, unique FD values
    bool                 is_ground   = false;
    bool                 is_free     = false;  // no domain / no constraint
    uint64_t             id          = 0;      // opaque caller-side id
};

// `narrow_fn(id, new_domain)` installs a pruned FD domain for var `id`.
// Called only for vars that were non-free & non-ground.  Must be trailed.
// Return `false` → signal user-level inconsistency, propagator fails.
using AlldiffNarrowFn =
    std::function<bool(uint64_t /*id*/, const std::vector<int64_t>& /*new_domain*/)>;

// Core algorithm entry point.  Mutates nothing in `vars`; reports pruning
// via `narrow`.
inline bool run_regin_alldiff(std::vector<AlldiffVar>& vars,
                              const AlldiffNarrowFn& narrow)
{
    // ── Step 0: feasibility / trivia ─────────────────────────────────────
    // - Two ground vars with the same value → fail.
    // - Skip "free" vars (no domain): Régin cannot prune against unbounded
    //   partners, but must still ensure ground values are removed from any
    //   constrained sibling.  We therefore split vars into three sets:
    //   ground, domained, free.  Regular matching applies to `domained`.
    std::vector<int64_t> ground_values;
    ground_values.reserve(vars.size());
    for (auto& v : vars) {
        if (v.is_ground) ground_values.push_back(v.ground_val);
    }
    std::sort(ground_values.begin(), ground_values.end());
    for (std::size_t i = 1; i < ground_values.size(); ++i) {
        if (ground_values[i] == ground_values[i - 1]) return false;  // dup
    }

    // Prune ground values from every non-ground domained var's domain.
    for (auto& v : vars) {
        if (v.is_ground || v.is_free) continue;
        std::vector<int64_t> filtered;
        filtered.reserve(v.domain.size());
        for (auto d : v.domain) {
            if (!std::binary_search(ground_values.begin(),
                                    ground_values.end(), d)) {
                filtered.push_back(d);
            }
        }
        if (filtered.empty()) return false;
        if (filtered.size() != v.domain.size()) {
            if (!narrow(v.id, filtered)) return false;
            v.domain = std::move(filtered);
        }
    }

    // ── Collect the vars we run the Régin matching on ───────────────────
    std::vector<std::size_t> xs_idx;         // indices into `vars`
    xs_idx.reserve(vars.size());
    for (std::size_t i = 0; i < vars.size(); ++i) {
        if (!vars[i].is_ground && !vars[i].is_free) xs_idx.push_back(i);
    }
    const std::size_t n = xs_idx.size();
    if (n <= 1) return true;                 // 0 or 1 var → nothing to do

    // Value id allocation — consecutive from 0.
    std::unordered_map<int64_t, int> val_id;
    std::vector<int64_t>             id_val;
    id_val.reserve(vars.size() * 4);
    auto intern = [&](int64_t v) -> int {
        auto it = val_id.find(v);
        if (it != val_id.end()) return it->second;
        int id = static_cast<int>(id_val.size());
        val_id.emplace(v, id);
        id_val.push_back(v);
        return id;
    };

    // adj[x] = sorted list of value ids in D(x).
    std::vector<std::vector<int>> adj(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto& v = vars[xs_idx[i]];
        adj[i].reserve(v.domain.size());
        for (auto d : v.domain) adj[i].push_back(intern(d));
        std::sort(adj[i].begin(), adj[i].end());
    }
    const std::size_t m_vals = id_val.size();

    // ── Step 2: Maximum matching via augmenting paths ────────────────────
    std::vector<int> match_x(n, -1);             // x → val id
    std::vector<int> match_v(m_vals, -1);        // val id → x
    std::vector<char> visited;
    // Recursive augmenting-path DFS (iterative form with explicit stack
    // would be safer for very large n; for typical CSP sizes O(n ≤ 1000)
    // the recursion is fine).
    std::function<bool(int)> try_augment = [&](int x) -> bool {
        for (int v : adj[x]) {
            if (visited[v]) continue;
            visited[v] = 1;
            if (match_v[v] == -1 || try_augment(match_v[v])) {
                match_x[x] = v;
                match_v[v] = x;
                return true;
            }
        }
        return false;
    };
    for (std::size_t x = 0; x < n; ++x) {
        visited.assign(m_vals, 0);
        if (!try_augment(static_cast<int>(x))) return false;   // infeasible
    }

    // ── Step 3: Build directed graph D ──────────────────────────────────
    // Node layout: first n nodes are vars, next m_vals nodes are values.
    const std::size_t V  = n + m_vals;
    auto var_node = [&](int x) { return static_cast<int>(x); };
    auto val_node = [&](int v) { return static_cast<int>(n + v); };

    std::vector<std::vector<int>> g(V);
    for (std::size_t x = 0; x < n; ++x) {
        for (int v : adj[x]) {
            if (match_x[x] == v) {
                // Matched: x → v
                g[var_node(static_cast<int>(x))].push_back(val_node(v));
            } else {
                // Unmatched: v → x
                g[val_node(v)].push_back(var_node(static_cast<int>(x)));
            }
        }
    }

    // ── Step 4: Free values set F ────────────────────────────────────────
    std::vector<char> is_free_val(m_vals, 0);
    std::vector<int>  free_vals;
    for (std::size_t v = 0; v < m_vals; ++v) {
        if (match_v[v] == -1) { is_free_val[v] = 1; free_vals.push_back(static_cast<int>(v)); }
    }

    // ── Step 5: Tarjan SCCs (iterative) ─────────────────────────────────
    std::vector<int>  comp(V, -1);
    std::vector<int>  idx(V, -1);
    std::vector<int>  lowlink(V, 0);
    std::vector<char> onstack(V, 0);
    std::vector<int>  stack;
    int next_index = 0, next_comp = 0;

    // Iterative Tarjan using an explicit work stack.
    struct Frame { int node; std::size_t cursor; };
    std::vector<Frame> work;
    for (int root = 0; root < static_cast<int>(V); ++root) {
        if (idx[root] != -1) continue;
        work.push_back({ root, 0 });
        idx[root]     = next_index;
        lowlink[root] = next_index;
        ++next_index;
        stack.push_back(root);
        onstack[root] = 1;
        while (!work.empty()) {
            auto& f = work.back();
            int u = f.node;
            if (f.cursor < g[u].size()) {
                int w = g[u][f.cursor++];
                if (idx[w] == -1) {
                    idx[w]     = next_index;
                    lowlink[w] = next_index;
                    ++next_index;
                    stack.push_back(w);
                    onstack[w] = 1;
                    work.push_back({ w, 0 });
                } else if (onstack[w]) {
                    if (idx[w] < lowlink[u]) lowlink[u] = idx[w];
                }
            } else {
                // All successors processed — pop and maybe emit SCC.
                work.pop_back();
                if (!work.empty()) {
                    int parent = work.back().node;
                    if (lowlink[u] < lowlink[parent]) lowlink[parent] = lowlink[u];
                }
                if (lowlink[u] == idx[u]) {
                    // Pop SCC.
                    while (true) {
                        int w = stack.back();
                        stack.pop_back();
                        onstack[w] = 0;
                        comp[w] = next_comp;
                        if (w == u) break;
                    }
                    ++next_comp;
                }
            }
        }
    }

    // ── Step 6: Reachability from free values (forward BFS) ──────────────
    std::vector<char> reached(V, 0);
    std::vector<int>  bfs;
    for (int v : free_vals) {
        int node = val_node(v);
        if (!reached[node]) { reached[node] = 1; bfs.push_back(node); }
    }
    for (std::size_t head = 0; head < bfs.size(); ++head) {
        int u = bfs[head];
        for (int w : g[u]) if (!reached[w]) { reached[w] = 1; bfs.push_back(w); }
    }

    // ── Step 7: Prune non-vital edges ───────────────────────────────────
    // Rebuild each var's domain keeping only "vital" values.
    for (std::size_t x = 0; x < n; ++x) {
        std::vector<int64_t> new_dom;
        new_dom.reserve(adj[x].size());
        for (int v : adj[x]) {
            // Vitality: matched, same-SCC, or v reachable from a free value.
            bool keep =
                (match_x[x] == v) ||
                (comp[var_node(static_cast<int>(x))] == comp[val_node(v)]) ||
                reached[val_node(v)];
            if (keep) new_dom.push_back(id_val[v]);
        }
        if (new_dom.empty()) return false;
        // Only narrow when the domain actually shrinks.
        if (new_dom.size() != adj[x].size()) {
            std::sort(new_dom.begin(), new_dom.end());
            if (!narrow(vars[xs_idx[x]].id, new_dom)) return false;
        }
    }
    return true;
}

} // namespace eta::runtime::clp

