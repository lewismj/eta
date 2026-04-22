# Eta Stdlib Style Guide

[<- Back to README](../README.md)

This guide captures the conventions used across `stdlib/std/*.eta`.
It is the practical baseline for new stdlib contributions.

## Naming

- Predicates end with `?` (`list?`, `tabled-rel?`).
- Mutators end with `!` (`assert-fact!`, `fact-table-insert!`).
- Conversions use `->` (`display->string`, `vector->list`).
- Internal helpers use `%` prefix and are not exported.
- Use canonical Scheme names when available; keep Eta-only convenience APIs explicitly documented.

## Core Style

- Use `(else ...)` for `cond` fallbacks, not `(#t ...)`.
- Prefer named-`let` for tail-recursive loops.
- Prefer `when` / `unless` for one-armed side-effect conditionals.
- Use `case` for symbol dispatch.
- Keep module exports grouped by section headers.

## Macros

- Use `define-syntax` with `syntax-rules` (hygienic macros only).
- Use macros for repeated syntax templates and binding forms.
- Keep runtime procedural APIs when dynamic/computed forms are required.
- If a macro also needs a module-facing API, provide a procedural wrapper.

## Error and Contracts

- Use consistent error tags/messages (`module:proc` style) for precondition violations.
- Logic goals should fail with `#f` on ordinary search failure, not raise exceptions.

## Documentation

- Module prose uses `;;;` comments.
- Local implementation notes use `;;` comments.
- Document deprecated aliases in the owning module under a dedicated section.
