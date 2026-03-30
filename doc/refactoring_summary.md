# Refactoring Changes Summary

This document summarizes the changes made to address the six concerns identified in the codebase review.

## 1. Redundant Desugaring Logic ✓

**Problem:** Both `expander.cpp` and `semantic_analyzer.cpp` implemented desugaring for `let`, `letrec`, `case`, and `do`.

**Solution:**
- Added comprehensive documentation to `expander.h` establishing the Expander as the **canonical** place for all desugaring
- Marked the derived form handlers in `semantic_analyzer.cpp` as **DEPRECATED**
- Added detailed comments explaining which forms should be desugared and what the Core IR output should look like
- The semantic analyzer handlers are retained for backwards compatibility but will be removed in a future version

**Files Changed:**
- `eta/reader/expander.h` - Added class-level documentation explaining desugaring transformations
- `eta/semantics/semantic_analyzer.cpp` - Added deprecation comments to derived form handlers
- `eta/semantics/semantic_analyzer.h` - Added documentation explaining expected input forms

## 2. Redundant GC Visitor Logic ✓

**Problem:** `mark_sweep_gc.h` had a manual `MarkVisitor` class that duplicated dispatch logic from `heap_visit.h`.

**Solution:**
- Created `LambdaHeapVisitor<Callback>` template that wraps any callable
- Added `visit_heap_refs()` convenience function for traversing heap object references
- Refactored `mark_from_roots()` to use the lambda-based visitor instead of a dedicated class
- When new heap types are added, they only need to be updated in `heap_visit.h`

**Files Changed:**
- `eta/runtime/memory/mark_sweep_gc.h` - Replaced `MarkVisitor` class with lambda-based approach

## 3. Manual Memory Management in IR ✓

**Problem:** `core_ir.h` uses raw `Node*` pointers, making IR transformations risky.

**Solution:**
- Created `eta/semantics/arena.h` with:
  - `Arena` class - block-based allocator for stable node addresses
  - `Ref<T>` wrapper - non-owning smart pointer for type safety
- Updated `core_ir.h` to:
  - Include the arena header
  - Add extensive documentation about ownership model
  - Export `NodeRef = Ref<Node>` for convenience
- Updated `semantic_analyzer.h` to document the ownership model of `ModuleSemantics`

**Files Created:**
- `eta/semantics/arena.h`

**Files Changed:**
- `eta/semantics/core_ir.h` - Added documentation and `NodeRef` alias
- `eta/semantics/semantic_analyzer.h` - Added ownership documentation

## 4. Lexer Complexity and Duplication ✓

**Problem:** Numeric parsing in `lexer.cpp` had duplicated logic across multiple methods.

**Solution:**
- Created `eta/reader/numeric.h` with:
  - `NumericParser` class - unified state machine for parsing all numeric formats
  - `NumericParseResult` struct - result type with kind, text, radix, and error message
  - `is_valid_digit()` - centralized digit validation for any radix
  - `is_special_float()` - IEEE literal detection
  - `parse_number()` - convenience function
  - `is_valid_integer()` and `is_valid_decimal()` - validation helpers

**Files Created:**
- `eta/reader/numeric.h`

**Note:** The lexer.cpp can be incrementally migrated to use these helpers. The new module provides a cleaner API that handles:
- Optional signs uniformly
- All radixes (2, 8, 10, 16)
- Decimal floats with exponents
- Special IEEE values (+inf.0, -inf.0, +nan.0, -nan.0)

## 5. Inconsistent String Handling ✓

**Problem:** String handling (interned vs heap) was inconsistent across the codebase.

**Solution:**
- Created `eta/runtime/string_view.h` with:
  - `StringView` class - unified abstraction for both interned and heap strings
  - `StringView::from()` - create from LispVal with error handling
  - `StringView::is_string()` - type check for any string variant
  - `StringView::equal()` - compare two strings of any kind
  - `is_symbol()` and `get_symbol_name()` - symbol utilities
- The class provides implicit conversion to `std::string_view`
- Tracks whether the string is interned or heap-allocated

**Files Created:**
- `eta/runtime/string_view.h`

**Usage Example:**
```cpp
auto sv = StringView::from(val, intern_table, heap);
if (sv) {
    std::string_view str = sv->view();
    // or use implicit conversion
    std::cout << *sv;
}
```

## 6. Fragmented Error Reporting ✓

**Problem:** Error structures were scattered across multiple files with no unified system.

**Solution:**
- Created `eta/diagnostic/diagnostic.h` with:
  - `Severity` enum - Error, Warning, Note, Hint
  - `DiagnosticCode` enum - unified error codes across all phases (lexer 0-99, parser 100-199, expander 200-299, linker 300-399, semantic 400-499, runtime 500-599)
  - `Diagnostic` struct - unified error with code, severity, span, message, and related spans
  - `DiagnosticEngine` class - accumulates diagnostics during compilation
  - `format_diagnostic()` - formatting with optional ANSI colors
  - `write_span()` - centralized span formatting
  - `DiagResult<T>` - result type alias using Diagnostic

**Files Created:**
- `eta/diagnostic/diagnostic.h`

**Migration Path:**
1. Individual error types (LexError, ExpandError, SemanticError) can include a `to_diagnostic()` specialization
2. Gradually migrate error handling to use `DiagnosticEngine`
3. Eventually remove the old error types

---

## Files Created

| File | Purpose |
|------|---------|
| `eta/diagnostic/diagnostic.h` | Unified diagnostic system |
| `eta/semantics/arena.h` | Arena allocator and Ref wrapper |
| `eta/reader/numeric.h` | Unified numeric parsing |
| `eta/runtime/string_view.h` | Unified string abstraction |

## Files Modified

| File | Changes |
|------|---------|
| `eta/runtime/memory/mark_sweep_gc.h` | Lambda-based visitor |
| `eta/semantics/core_ir.h` | Arena integration, documentation |
| `eta/semantics/semantic_analyzer.h` | Ownership documentation |
| `eta/semantics/semantic_analyzer.cpp` | Deprecation comments |
| `eta/reader/expander.h` | Desugaring documentation |

## Next Steps

All originally planned migration steps have been completed:

### ✓ Lexer Migration (COMPLETED)
- `lexer.cpp` now uses `numeric.h` functions for:
  - `classify_number()` - uses `numeric::parse_number()`
  - `is_valid_digit_for_radix()` - uses `numeric::is_valid_digit()`
  - `is_valid_decimal()` - uses `numeric::is_valid_decimal()`
  - `is_signed_integer()` - uses `numeric::is_valid_integer()`
  - `try_collect_inf_nan()` - uses `numeric::is_special_float()`

### ✓ VM String Migration (COMPLETED)
- Added `StringView` include and helper functions to `vm.cpp`
- Helper functions available: `is_string_value()`, `get_string_value()`, `string_equal()`
- Note: StringView is included in .cpp file (not .h) to avoid circular dependency with `continuation.h`

### ✓ Diagnostic Migration (COMPLETED)
- Added `to_diagnostic<>` template specializations for:
  - `LexError` - maps to lexer diagnostic codes (0-99)
  - `ExpandError` - maps to expander diagnostic codes (200-299)
  - `SemanticError` - maps to semantic diagnostic codes (400-499)
- All error types can now be converted to unified `Diagnostic` objects

### ✓ Arena Migration (COMPLETED)
- `ModuleSemantics` now uses `Arena` for node allocation
- `emplace<>()` now allocates from arena for stable pointers and better cache performance
- Legacy `emplace_legacy<>()` method retained but marked deprecated
- `nodes` deque kept for backwards compatibility but should not be used

### ✓ Remove Deprecated Handlers (COMPLETED)
- Removed `handle_let()`, `handle_letrec()`, `handle_case()`, `handle_do()` handlers
- Removed `build_loop_call()` and `create_lambda_with_scope()` helper functions
- Updated handler registry to only include core forms
- Derived forms MUST now be desugared by the Expander before semantic analysis
- If derived forms reach semantic analyzer, they will be treated as function applications (producing meaningful errors)

## Future Considerations

1. **Full diagnostic adoption**: Replace all phase-specific error returns with `DiagResult<T>`
2. **Source file registry**: Add file content storage for rich error display with source snippets
3. **LSP integration**: Expose `DiagnosticEngine` for editor integration
4. **Remove legacy deque**: After confirming arena works correctly, remove `nodes` deque entirely


