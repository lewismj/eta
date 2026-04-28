# NaN-Boxing Memory Layout

[← Back to README](../../../README.md) · [Architecture](../../architecture.md) ·
[Bytecode & VM](bytecode-vm.md) · [Runtime & GC](runtime.md)

---

## Overview

Every value in Eta is a **64-bit `LispVal`** (a `uint64_t`). Regular IEEE-754
`double` values pass through unmodified, while all other types — integers,
characters, symbols, strings, booleans, nil, and heap pointers — are packed
into the unused NaN bit patterns. This technique is called **NaN-boxing**.

The result: no virtual dispatch, no tagged union overhead, no pointer
indirection for common values, and doubles require **zero encoding cost**.

**Source:** [`nanbox.h`](../../../eta/core/src/eta/runtime/nanbox.h)

---

## IEEE-754 Double Recap

A 64-bit IEEE-754 double has this structure:

```
  63  62       52  51                                             0
 ┌───┬───────────┬──────────────────────────────────────────────────┐
 │ S │ Exponent  │                   Mantissa                       │
 │ 1 │  11 bits  │                   52 bits                        │
 └───┴───────────┴──────────────────────────────────────────────────┘
```

A value is **NaN** when all 11 exponent bits are `1` and the mantissa is
non-zero. IEEE-754 defines two flavours:

- **Signaling NaN (sNaN):** bit 51 = `0`, mantissa ≠ 0
- **Quiet NaN (qNaN):** bit 51 = `1`

Hardware only ever produces **one** canonical qNaN pattern, leaving
~2⁵¹ bit patterns free. Eta claims a subset of these for tagged values.

---

## Eta's Bit Layout

For a **boxed** (non-double) value, bits 62–50 are fixed as
`11111111111 1 1` (exponent all-ones + QNaN bit + marker bit). The
remaining 50 bits are split into a **3-bit tag** and a **47-bit payload**:

```
  63  62       52  51  50  49  48  47  46                          0
 ┌───┬───────────┬───┬───┬─────────┬────────────────────────────────┐
 │ S │ 111 1111 1111│ Q │ M │  Tag │            Payload             │
 │ ? │ (Exponent)│ 1 │ 1 │ 3 bits  │           47 bits              │
 └───┴───────────┴───┴───┴─────────┴────────────────────────────────┘
      ◄─── BOXED_PATTERN_MASK ──►
            0x7FFC000000000000
```

| Field | Bits | Value | Purpose |
|-------|------|-------|---------|
| Exponent | 62–52 | `0x7FF` | All ones = NaN/Inf region |
| Q (Quiet NaN) | 51 | `1` | Marks as quiet NaN (avoids FP traps) |
| M (Marker) | 50 | `1` | Distinguishes Eta boxes from real NaN |
| Tag | 49–47 | 0–7 | Type discriminator |
| Payload | 46–0 | varies | Type-specific data |

### The `is_boxed` Check

```cpp
constexpr bool is_boxed(uint64_t bits) {
    return (bits & BOXED_PATTERN_MASK) == BOXED_PATTERN_MASK;
}
// BOXED_PATTERN_MASK = 0x7FFC'0000'0000'0000
//   = QNAN_EXP_BITS | QNAN_BIT | MARKER_BIT
```

If `is_boxed()` is **false**, the 64 bits are a valid IEEE-754 `double`
(including `+Inf`, `-Inf`, and normal numbers). No decoding needed — just
`std::bit_cast<double>(bits)`.

---

## Tag Values

```cpp
enum class Tag : uint8_t {
    Nil        = 0,  // #f / '() / void
    Char       = 1,  // Unicode code point (up to U+10FFFF)
    Fixnum     = 2,  // 47-bit signed integer
    String     = 3,  // Intern ID → InternTable
    Symbol     = 4,  // Intern ID → InternTable
    Nan        = 5,  // Canonical NaN (the one real NaN value)
    HeapObject = 6,  // Object ID → Heap
};
```

---

## Encoding Examples

### Regular `double` (e.g., `3.14`)

Doubles are stored **as-is** — their IEEE-754 representation is not a
boxed NaN pattern, so `is_boxed()` returns false.

```
                3.14  →  0x40091EB851EB851F  (IEEE-754)
is_boxed check:  0x40091... & 0x7FFC... = 0x4008... ≠ 0x7FFC...  → NOT boxed
                 → decode<double> returns bit_cast<double>(bits) = 3.14  ✓
```

### Canonical NaN

All NaN variants (`NaN`, `-NaN`, signaling NaNs) are normalized to a
single boxed representation:

```
  NaN  →  box(Tag::Nan, 0)

  0x7FFC  0000  0000  0000
  │└────┘ └────────────────┘
  │  Exponent+Q+M   Payload = 0
  └ Sign = 0

  Bits: 0 11111111111 1 1 101 00000000000000000000000000000000000000000000000
                      Q M Tag=5
```

### Fixnum `42`

```
  42  →  box(Tag::Fixnum, 42)

  0x7FFC  8000  0000  002A
                            │
  Bits: 0 11111111111 1 1 010 00000000000000000000000000000000000000000101010
                      Q M Tag=2                                         42
```

**Range:** 47-bit signed → `FIXNUM_MIN = -70,368,744,177,664` to
`FIXNUM_MAX = 70,368,744,177,663` (±2⁴⁶). Integers outside this range
are heap-allocated as big fixnums.

### Fixnum `-1`

Negative fixnums use two's-complement within the 47-bit payload:

```
  -1  →  box(Tag::Fixnum, 0x7FFFFFFFFFFF)   // all 47 bits set

  Bits: 0 11111111111 1 1 010 11111111111111111111111111111111111111111111111
                      Q M Tag=2              47 bits all 1s = -1 (two's complement)
```

Sign-extension on decode:
```cpp
constexpr int64_t sign_extend_fixnum(uint64_t raw_payload) {
    return (raw_payload & FIXNUM_SIGN_BIT)                // bit 46 set?
        ? static_cast<int64_t>(raw_payload | FIXNUM_SIGN_EXTEND_MASK)
        : static_cast<int64_t>(raw_payload);
}
```

### Character `#\λ` (U+03BB)

```
  'λ'  →  box(Tag::Char, 0x03BB)

  0x7FFC  4000  0000  03BB
  Bits: 0 11111111111 1 1 001 00000000000000000000000000000000001110111011
                      Q M Tag=1                                     U+03BB
```

The full Unicode range (U+0000 to U+10FFFF) fits in 21 bits, well within
the 47-bit payload.

### Symbol `hello`

Symbols are interned strings. The payload is an `InternId` from the
`InternTable`:

```
  intern("hello")  →  InternId = 7

  box(Tag::Symbol, 7)  →  0x7FFD 0000 0000 0007
  Bits: 0 11111111111 1 1 100 00000000000000000000000000000000000000000000111
                      Q M Tag=4                                           7
```

At runtime, the string content is retrieved via
`intern_table.get_string(7)  →  "hello"`.

### String `"world"`

Strings use the same interning mechanism but with a different tag:

```
  intern("world")  →  InternId = 12

  box(Tag::String, 12)  →  0x7FFC C000 0000 000C
  Bits: 0 11111111111 1 1 011 00000000000000000000000000000000000000000001100
                      Q M Tag=3                                          12
```

### Nil / True / False

```
  Nil   =  box(Tag::Nil, 0)     // 0x7FFC'0000'0000'0000
  False =  Nil                   // #f is identical to nil
  True  =  box(Tag::Nil, 1)     // 0x7FFC'0000'0000'0001
```

Truthiness: any value that is not `Nil` (i.e., not `False`) is truthy.

### Heap Object (e.g., a Cons cell)

Heap-allocated objects store their `ObjectId` in the payload:

```
  heap.allocate<Cons>(...)  →  ObjectId = 42

  box(Tag::HeapObject, 42)  →  0x7FFD 8000 0000 002A
  Bits: 0 11111111111 1 1 110 00000000000000000000000000000000000000000101010
                      Q M Tag=6                                         42
```

At runtime, the object is retrieved via `heap.try_get_as<Cons>(42)`.

---

## Constant Masks Reference

| Constant | Hex | Description |
|----------|-----|-------------|
| `EXPONENT_MASK` | `0x7FF` | 11-bit exponent field (after >> 52) |
| `MANTISSA_MASK` | `0x000FFFFFFFFFFFFF` | Lower 52 bits |
| `QNAN_EXP_BITS` | `0x7FF0000000000000` | Exponent = all ones |
| `QNAN_BIT` | `0x0008000000000000` | Bit 51 — quiet NaN |
| `MARKER_BIT` | `0x0004000000000000` | Bit 50 — Eta marker |
| `BOXED_PATTERN_MASK` | `0x7FFC000000000000` | `QNAN_EXP_BITS \| QNAN_BIT \| MARKER_BIT` |
| `TAG_SHIFT` | `47` | Shift to extract 3-bit tag |
| `TAG_MASK` | `0x07` | Mask for 3-bit tag |
| `PAYLOAD_MASK` | `0x00007FFFFFFFFFFF` | Lower 47 bits |
| `FIXNUM_MIN` | `-70368744177664` | −2⁴⁶ |
| `FIXNUM_MAX` | `70368744177663` | 2⁴⁶ − 1 |
| `UNICODE_MAX` | `0x10FFFF` | Maximum Unicode code point |
| `FIXNUM_SIGN_BIT` | `0x0000400000000000` | Bit 46 |

---

## Encoding / Decoding API

The NaN-box API is fully `constexpr` and uses `std::expected` for error
handling:

```cpp
// Encode a C++ value into a LispVal
auto val = nanbox::ops::encode<int64_t>(42);       // → expected<LispVal, NaNBoxError>
auto flt = nanbox::ops::encode<double>(3.14);      // → expected<LispVal, NaNBoxError>
auto chr = nanbox::ops::encode<char32_t>(U'λ');    // → expected<LispVal, NaNBoxError>

// Decode back
auto i = nanbox::ops::decode<int64_t>(val.value()); // → expected<int64_t, NaNBoxError>
auto d = nanbox::ops::decode<double>(flt.value());   // → expected<double, NaNBoxError>

// Type inspection
nanbox::ops::is_boxed(val.value());  // true  — it's a tagged fixnum
nanbox::ops::is_boxed(flt.value());  // false — it's a raw double
nanbox::ops::tag(val.value());       // Tag::Fixnum
```

---

## Design Rationale

1. **Doubles are free.** The most common numeric type incurs zero
   encoding/decoding cost — it's just a reinterpretation of the same bits.

2. **47-bit fixnums cover most integers.** The range ±70 trillion handles
   virtually all practical integer use. Only overflow cases spill to the heap.

3. **Single 64-bit word.** No tagged-pointer indirection, no discriminated
   union overhead. Values fit in registers, arrays of `LispVal` are dense,
   and the GC root scanner only needs to check `is_boxed() && tag() == HeapObject`.

4. **`std::expected` for safety.** Encoding validates range (fixnum bounds,
   Unicode range) and returns `NaNBoxError::OutOfRange` instead of
   silently truncating.

