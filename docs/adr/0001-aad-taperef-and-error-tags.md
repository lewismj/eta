# ADR 0001: TapeRef Encoding, Generation, and AD Error Tags

Date: 2026-04-23
Status: Accepted

## Context

Eta's tape-based AAD needed deterministic safety checks for:

- Cross-tape misuse
- Stale references after tape reset/clear
- Wrong active-tape lookup
- Cross-VM transport of VM-local AD values

The previous TapeRef payload stored only a node index, so ownership and lifecycle
state were implicit.

## Decision

### TapeRef representation

TapeRef remains a NaN-boxed immediate (`Tag::TapeRef`) with a packed payload:

- `tape_id` (13 bits)
- `generation` (10 bits)
- `node_index` (24 bits)

Layout in payload bits:

`[ tape-id:13 | generation:10 | node-index:24 ]`

`tape_id = 0` is reserved as invalid.

### Ownership and generation behavior

- Every tape is assigned a non-zero `tape_id`.
- `generation` starts at 1 and is normalized to `[1, MAX_GENERATION]`.
- `tape-clear!` clears entries and bumps generation.
- Any TapeRef lookup validates:
  - `tape_id` match
  - `generation` match
  - `node_index` in bounds

### Equality and hashing semantics

TapeRefs are equal only when all three packed fields match:

- `tape_id`
- `generation`
- `node_index`

Refs from old generations are never equal to refs from the current generation.

### Canonical AD runtime error tags

The AD runtime uses this fixed tag vocabulary:

- `:ad/mixed-tape`
- `:ad/stale-ref`
- `:ad/no-active-tape`
- `:ad/nondiff-strict`
- `:ad/cross-vm-ref`
- `:ad/domain`

Each AD error carries structured fields (for example `op`, tape ids,
generations, node index, and path metadata for cross-VM violations).

## Consequences

- Cross-tape and stale-reference failures are deterministic and catchable.
- Active-tape lookups now fail with explicit AD tags instead of silent misuse.
- Message boundaries reject `Tape` and `TapeRef` values with `:ad/cross-vm-ref`.
- Tests can assert on tag identity and payload fields rather than string text.

## Migration Notes

- `tape-ref-value-of` is the explicit primitive for reading a TapeRef against a
  specific tape.
- `tape-ref-value` remains available but is strict for TapeRef arguments and
  fails if no matching active tape is present.
- Branching logic should use explicit primal extraction helpers where possible.

