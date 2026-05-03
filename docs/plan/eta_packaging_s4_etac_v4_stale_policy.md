## Eta Packaging S4 `.etac` v4 Metadata + Stale-Artifact Policy

This note records the S4 ".etac v4 metadata + stale-artifact policy"
implementation from `docs/plan/eta_packaging_plan.md`.

### Landed scope

- Bumped `.etac` format from v3 to v4 in `BytecodeSerializer`.
- Added v4 header metadata:
  - compiler fingerprint (`compiler_id`, 16 bytes)
  - optional package metadata (`name`, `version`, `manifest_hash`)
  - optional dependency hash table (`dependency -> etac_hash`)
- Kept backward-compatible v3 reader support:
  - deserializer now accepts both v3 and v4 artifacts
  - v3 artifacts are parsed without compiler/package metadata
- Added stale-artifact freshness API:
  - `FreshnessContext`, `FreshnessStatus`, `FreshnessResult`
  - deterministic checks for compiler, builtins, source hash,
    manifest hash, and dependency hash mismatches
- Integrated freshness policy in runtime `.etac` loading:
  - `Driver::run_etac_file` now checks freshness after deserialization
  - stale artifacts with sibling `.eta` source fall back to source load
    with a warning diagnostic
  - stale artifacts without source fallback emit a hard error diagnostic
- Extended S4 coverage in tests:
  - `bytecode_serializer_tests`: v4 metadata round-trip,
    v3 compatibility read path, builtin mismatch, compiler mismatch,
    and stale-detection statuses
  - `packaging_contract_tests`: stale source-hash detection with
    source fallback behavior in `run_etac_file`

### S4 test gate

The S4 gate is satisfied when all of these are green:

- `eta_core_test`
- `eta_test`
- `eta_pkg_test`
- `eta_cli_test`

