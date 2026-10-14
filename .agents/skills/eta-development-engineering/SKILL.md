---
name: eta-development-engineering
description: >-
  Engineering standards and build/test workflow for the Eta repository.
  Use when implementing features, fixing bugs, refactoring code,
  or changing tests, build configuration, or documentation.
---

# Development Engineering Standards

## 1. General principles

- Follow the existing architecture, coding style, naming conventions, and
  established patterns in the repository.
- Prefer the smallest complete, maintainable change that correctly solves
  the problem.
- Understand the relevant implementation and its callers before modifying
  code. Do not make changes based on assumptions when the source can be
  inspected.
- Preserve existing behaviour unless the task explicitly requires a change.
- Avoid unrelated refactoring and unnecessary scope expansion.
- Do not duplicate existing functionality or introduce abstractions without
  a clear justification.
- Do not claim that a change is correct, a build succeeded, or tests passed
  unless the corresponding checks were actually performed.

## 2. Build environments

Use the following CLion-compatible CMake configurations for the Eta project.
Keep the Windows and WSL build directories separate.

### Windows Release

Configure:

```powershell
& "C:\Program Files\JetBrains\CLion 2026.1\bin\cmake\win\x64\bin\cmake.exe" `
  -DCMAKE_BUILD_TYPE=Release `
  "-DCMAKE_MAKE_PROGRAM=C:/Program Files/JetBrains/CLion 2026.1/bin/ninja/win/x64/ninja.exe" `
  -G Ninja `
  -S C:\Users\lewis\develop\eta `
  -B C:\Users\lewis\develop\eta\cmake-build-release-visual-studio
```

Build:

```powershell
& "C:\Program Files\JetBrains\CLion 2026.1\bin\cmake\win\x64\bin\cmake.exe" `
  --build C:\Users\lewis\develop\eta\cmake-build-release-visual-studio `
  --parallel
```

### Windows Debug

Configure:

```powershell
& "C:\Program Files\JetBrains\CLion 2026.1\bin\cmake\win\x64\bin\cmake.exe" `
  -DCMAKE_BUILD_TYPE=Debug `
  "-DCMAKE_MAKE_PROGRAM=C:/Program Files/JetBrains/CLion 2026.1/bin/ninja/win/x64/ninja.exe" `
  -G Ninja `
  -S C:\Users\lewis\develop\eta `
  -B C:\Users\lewis\develop\eta\cmake-build-debug-visual-studio
```

Build:

```powershell
& "C:\Program Files\JetBrains\CLion 2026.1\bin\cmake\win\x64\bin\cmake.exe" `
  --build C:\Users\lewis\develop\eta\cmake-build-debug-visual-studio `
  --parallel
```

### WSL Release

Configure:

```bash
/usr/local/bin/cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/gmake \
  -G "Unix Makefiles" \
  -S /mnt/c/Users/lewis/develop/eta \
  -B /mnt/c/Users/lewis/develop/eta/cmake-build-release
```

Build:

```bash
/usr/local/bin/cmake \
  --build /mnt/c/Users/lewis/develop/eta/cmake-build-release \
  --parallel
```

### WSL Debug

Configure:

```bash
/usr/local/bin/cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/gmake \
  -G "Unix Makefiles" \
  -S /mnt/c/Users/lewis/develop/eta \
  -B /mnt/c/Users/lewis/develop/eta/cmake-build-debug
```

Build:

```bash
/usr/local/bin/cmake \
  --build /mnt/c/Users/lewis/develop/eta/cmake-build-debug \
  --parallel
```

### Build rules

- Reuse an existing build directory when its generator, compiler, toolchain,
  and configuration are compatible with the intended build.
- Prefer incremental builds using the existing, compatible build directory.
  Do not delete or recreate build directories unless necessary and justified.
- Do not switch generators or toolchains inside an existing CMake build
  directory. Use a separate build directory when necessary.
- Do not change the compiler, generator, toolchain, or build configuration
  without a task-related reason.
- Reconfigure when build-system inputs have changed or when required by the
  task.
- Do not assume that a successful configuration means compilation succeeded.
- Use the build environment appropriate to the task. Run additional
  configurations when cross-platform compatibility or configuration-specific
  behaviour is relevant.
- If a build fails, capture and diagnose the first meaningful error before
  attempting corrective changes.
- Do not modify project build settings merely to work around an unexplained
  local build failure. Diagnose the cause first.

## 3. Testing and verification

- Every feature, bug fix, or behaviour change must have appropriate test
  coverage.
- Add regression tests for bugs so that the original failure is reproducible
  and the fix is verified.
- Cover relevant edge cases, invalid inputs, failure paths, and boundary
  conditions.
- Follow the existing test framework, conventions, and test organisation.
- Run the relevant targeted tests after making changes.
- Run the complete applicable test suite before declaring the task complete.
  If this is impractical, explain why, run the broadest reasonable subset,
  and report exactly what remains unverified.
- Build and test the configurations affected by the change.
- If a test fails, investigate whether the failure was introduced by the
  change. Do not dismiss failures without evidence.
- Never disable, delete, weaken, or skip a test merely to obtain a passing
  test run.
- Do not replace meaningful assertions with weaker checks to make tests pass.
- If a required test cannot be run, state why and identify the unverified
  behaviour explicitly.

## 4. No temporary workarounds

Do not introduce temporary workarounds, shims, hacks, or compatibility
layers as substitutes for a correct implementation.

In particular:

- Do not hard-code values to conceal an underlying defect.
- Do not silently swallow errors or suppress diagnostics to make a build pass.
- Do not bypass validation, invariants, or error handling to avoid fixing
  the underlying problem.
- Do not introduce arbitrary delays, retries, fallbacks, or special cases
  without a justified requirement.
- Do not add redundant code paths or abstractions to avoid correcting an
  existing implementation.
- Do not leave commented-out code, temporary debugging instrumentation,
  placeholder implementations, or unexplained TODOs in the patch.
- Do not alter unrelated functionality to make a failing test pass.

A workaround is acceptable only when explicitly required by the task or
justified by a genuine external constraint. Document the reason, limitations,
and intended resolution.

Prefer a correct root-cause fix over a symptom-level patch.

## 5. Coding standards and maintainability

- Match the conventions of the surrounding code rather than introducing a
  competing style.
- Follow the repository's established naming, formatting, include ordering,
  namespace, and file organisation conventions.
- Prefer clear interfaces, explicit contracts, and well-defined ownership.
- Preserve error-handling conventions and propagate failures appropriately.
- Avoid unnecessary allocations, copies, global state, and duplicated work
  where these conflict with the existing architecture or performance
  requirements.
- Do not sacrifice correctness or readability for speculative optimisation.
- Keep functions, types, and modules focused on their intended
  responsibilities.
- Remove unused code and unnecessary dependencies introduced by the change.
- Update public interfaces and their callers consistently.
- Do not introduce new third-party dependencies without a clear need.

## 6. Documentation and text encoding

- Ensure changed source files and documentation use the repository's
  established encoding, preferably UTF-8.
- Check changed files for mojibake, corrupted characters, malformed
  punctuation, and unintended encoding conversions.
- Preserve intentional Unicode characters and existing line-ending
  conventions where practical.
- Keep documentation consistent with the actual implementation.
- Update relevant documentation when public behaviour, configuration,
  interfaces, build instructions, or usage changes.
- Do not claim support for functionality that has not been implemented
  and verified.
- Check that code examples, commands, paths, and configuration snippets
  are syntactically correct and usable.

## 7. Scope and repository hygiene

- Inspect the working tree before editing so that existing user changes
  are not overwritten or mistaken for changes made by the task.
- Do not discard, reset, or overwrite unrelated user modifications.
- Avoid changing generated files, vendored code, build artefacts, or
  unrelated documentation unless explicitly required.
- Do not commit, push, tag, or publish changes unless explicitly requested.
- Review the final diff for accidental changes, whitespace problems,
  missing files, and unrelated modifications.

## 8. Completion criteria

Before declaring a task complete:

1. Review the implementation against the requested behaviour.
2. Review the final diff for correctness and scope.
3. Confirm that appropriate tests were added or updated.
4. Build the affected targets successfully.
5. Run the relevant tests and the complete applicable test suite. If the
   full suite cannot reasonably be run, document the reason and the remaining
   verification gap.
6. Check changed documentation and source files for encoding problems.
7. Confirm that no unjustified workarounds, disabled tests, temporary
   debugging code, or unexplained placeholders remain.
8. Report any remaining limitations or unverified behaviour honestly.

The final response must summarise:

- **Changes:** What was implemented or fixed.
- **Tests:** Which tests were run and their results.
- **Builds:** Which build configurations succeeded or failed.
- **Limitations:** Any outstanding issues or checks that could not be run.

Never report an unexecuted build or test as successful.
