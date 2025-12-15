#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/factory.h>

// Fuzz target for exercising the heap allocator and factory helpers.
// The goal is to cover: allocate/deallocate paths, soft-limit errors,
// and metadata access via try_get / with_entry.

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t sz) {
    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::nanbox::ops;
    using namespace eta::runtime::memory::heap;
    using namespace eta::runtime::memory::factory;

    if (!data || sz == 0) return 0;

    // Bound the number of operations and heap soft limit from input.
    const uint8_t b0 = data[0];
    const size_t ops = std::clamp<size_t>(b0 % 64, 8, 64); // 8..64 steps

    // Derive a soft limit: 64 KiB .. 2 MiB, biased by input size.
    const size_t min_limit = 64 * 1024;
    const size_t max_limit = 2 * 1024 * 1024;
    const size_t derived = min_limit + ((sz * 257u) % (max_limit - min_limit + 1));

    Heap heap(derived);

    // Keep track of values we created; for heap objects we may deallocate later.
    std::vector<LispVal> values;
    values.reserve(ops);

    auto make_large_fixnum = [&](uint64_t raw) {
        // Try to force heap path for fixnum: if in range, bump above FIXNUM_MAX.
        uint64_t v = raw;
        if (v <= static_cast<uint64_t>(constants::FIXNUM_MAX)) {
            v = static_cast<uint64_t>(constants::FIXNUM_MAX) + 1u;
        }
        auto r = make_fixnum(heap, v);
        if (r.has_value()) values.push_back(*r);
    };

    auto maybe_deallocate = [&](){
        if (values.empty()) return;
        const size_t idx = data[(values.size() + sz) % sz] % values.size();
        const LispVal v = values[idx];
        if (tag(v) == Tag::HeapObject) {
            const ObjectId id = payload(v);
            // Ignore the returned error to keep fuzzer going.
            (void)heap.deallocate(id);
            // Remove from our vector to avoid double-deallocation.
            values.erase(values.begin() + static_cast<long long>(idx));
        }
    };

    auto make_small_fixnum = [&](int64_t raw) {
        // Map into FIXNUM range to exercise immediate path
        int64_t span = constants::FIXNUM_MAX - constants::FIXNUM_MIN;
        int64_t v = constants::FIXNUM_MIN + (raw % (span == 0 ? 1 : span));
        auto r = make_fixnum(heap, v);
        if (r.has_value()) values.push_back(*r);
    };

    auto make_cons_from_pool = [&](){
        LispVal car = values.empty() ? box(Tag::Nil, 0) : values[data[(values.size() * 3 + 1) % sz] % values.size()];
        LispVal cdr = values.empty() ? box(Tag::Nil, 0) : values[data[(values.size() * 5 + 7) % sz] % values.size()];
        auto r = make_cons(heap, car, cdr);
        if (r.has_value()) values.push_back(*r);
    };

    auto make_lambda_from_pool = [&](){
        // Make a tiny lambda with a few captured values
        std::vector<LispVal> formals;
        std::vector<LispVal> upvals;

        const size_t nf = values.empty() ? 0 : (data[(values.size() * 11 + 3) % sz] % 3);
        const size_t nu = values.empty() ? 0 : (data[(values.size() * 13 + 9) % sz] % 4);

        for (size_t i = 0; i < nf; ++i) {
            formals.push_back(values[data[(i * 17 + 4) % sz] % values.size()]);
        }
        for (size_t i = 0; i < nu; ++i) {
            upvals.push_back(values[data[(i * 19 + 6) % sz] % values.size()]);
        }
        LispVal body = values.empty() ? box(Tag::Nil, 0) : values[data[(values.size() * 7 + 2) % sz] % values.size()];

        auto r = make_lambda(heap, std::move(formals), body, std::move(upvals));
        if (r.has_value()) values.push_back(*r);
    };

    auto touch_metadata = [&](){
        if (values.empty()) return;
        const LispVal v = values.back();
        if (tag(v) == Tag::HeapObject) {
            const ObjectId id = payload(v);
            // Snapshot entry
            HeapEntry entry{};
            (void)heap.try_get(id, entry);

            // Mutate only the allowed header.flags via with_entry
            (void)heap.with_entry(id, [](HeapEntry& e){
                e.header.flags ^= MARK_BIT; // toggle mark bit for coverage
            });
        }
        // Also read total bytes for some variability
        (void)heap.total_bytes();
    };

    // Walk through the input after b0 to drive operations
    size_t cursor = 1;
    auto next_byte = [&]() -> uint8_t {
        if (sz <= 1) return 0;
        uint8_t b = data[cursor % sz];
        cursor++;
        return b;
    };

    for (size_t i = 0; i < ops; ++i) {
        const uint8_t op = next_byte() % 8; // 8 kinds of operations
        switch (op) {
            case 0: {
                // Large unsigned fixnum (heap path)
                uint64_t u = 0;
                if (sz - (cursor % sz) >= sizeof(uint64_t)) {
                    // best-effort consume 8 bytes without reading past buffer
                    std::memcpy(&u, data + (cursor % sz), std::min(sizeof(uint64_t), sz - (cursor % sz)));
                    cursor += sizeof(uint64_t);
                } else {
                    u = (static_cast<uint64_t>(next_byte()) << 32) | next_byte();
                }
                make_large_fixnum(u);
                break;
            }
            case 1: {
                // Small signed fixnum (immediate path when in range)
                int64_t s = 0;
                if (sz - (cursor % sz) >= sizeof(int64_t)) {
                    std::memcpy(&s, data + (cursor % sz), std::min(sizeof(int64_t), sz - (cursor % sz)));
                    cursor += sizeof(int64_t);
                } else {
                    s = static_cast<int64_t>(static_cast<int8_t>(next_byte()));
                }
                make_small_fixnum(s);
                break;
            }
            case 2: {
                // Cons cell using values pool
                make_cons_from_pool();
                break;
            }
            case 3: {
                // Lambda using values pool
                make_lambda_from_pool();
                break;
            }
            case 4: {
                // Attempt to deallocate a random heap object we created
                maybe_deallocate();
                break;
            }
            case 5: {
                // Touch metadata of the latest item
                touch_metadata();
                break;
            }
            case 6: {
                // Mix: immediate Nil/Booleans for diversity
                const bool b = (next_byte() & 1) != 0;
                values.push_back(ops::encode(b).value());
                values.push_back(box(Tag::Nil, 0));
                break;
            }
            case 7: {
                // Create a few more fixnums quickly
                values.push_back(ops::encode(static_cast<int32_t>(next_byte())).value());
                values.push_back(ops::encode(static_cast<uint32_t>(next_byte())).value());
                break;
            }
            default:
                break;
        }

        // Keep vector from growing unbounded
        if (values.size() > 2048) {
            values.erase(values.begin(), values.begin() + static_cast<long long>(values.size() - 1024));
        }

        // If we exceeded soft limit, further allocations will error — that’s okay.
        // We continue to exercise deallocation and metadata operations.
    }

    // Final cleanup attempt: deallocate any remaining heap objects we created
    for (const auto v : values) {
        if (tag(v) == Tag::HeapObject) {
            const ObjectId id = payload(v);
            (void)heap.deallocate(id);
        }
    }

    return 0;
}
