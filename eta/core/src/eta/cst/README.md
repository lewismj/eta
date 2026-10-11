# Internal lossless CST library

Build target: `eta_cst` (static, not installed). Link it to obtain snapshot/source
maps, shared lexer scanning, tokens/trivia, tolerant concrete parsing, lossless
serialization, diagnostic adaptation and the fallible S-expression bridge.

See [the reader contract and inventory](../../../../../docs/cst_reader.md) for
ownership, reader modes, limits, tests and benchmark reproduction. `parse()`
requires an explicit `ReaderConfig`; retain the returned shared file for as
long as any source view or token/node ID is used.

To investigate a mismatch, check `has_unique_byte_ownership()`, compare
`read_original()` with `to_sexpr()`, then inspect token-owned trivia, exact
`discarded_end` boundaries and reader-state/path traces. Recovered/unsupported
files are ineligible for layout or semantic bridging.
