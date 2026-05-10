# Neural Networks (LibTorch)

Native sidecar package metadata for `torch/*`, `nn/*`, and `optim/*`
runtime primitives.

The lockfile is the runtime source of truth for:

1. selected target triple,
2. artifact relative path,
3. artifact checksum.

At materialization/load time Eta verifies the lockfile checksum against the
resolved artifact before dynamic loading.
