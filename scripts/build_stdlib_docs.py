#!/usr/bin/env python3
"""Extract stdlib symbol docs from ;;@doc blocks and emit a C++ registry."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


SUPPORTED_DOC_FORMS = {
    "defun",
    "define",
    "define-syntax",
    "defmacro",
    "define-macro",
}

TAG_CATEGORY = "@category "
TAG_MODULE = "@module "
TAG_ALIAS_OF = "@alias-of "
TAG_SINCE = "@since "
TAG_DEPRECATED = "@deprecated "
TAG_EXAMPLE = "@example"
TAG_DOC = "@doc "


@dataclass(frozen=True)
class Diagnostic:
    """Structured extractor diagnostic."""

    file: Path
    line: int
    message: str

    def format(self) -> str:
        return f"{self.file}:{self.line}: error: {self.message}"


@dataclass(frozen=True)
class ParsedDocBlock:
    """Parsed content of one ;;@doc block before symbol attachment."""

    line: int
    signature: str
    category: str
    module: str
    alias_of: str
    since: str
    deprecated: str
    body_lines: tuple[str, ...]
    examples: tuple[tuple[str, ...], ...]


@dataclass(frozen=True)
class ExtractedDoc:
    """Final extracted symbol doc record."""

    file: Path
    line: int
    name: str
    qualified_name: str
    signature: str
    summary: str
    details: str
    category: str
    module: str
    alias_of: str
    since: str
    deprecated: str
    examples: tuple[tuple[str, ...], ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src-root", required=True, help="Path to stdlib source root")
    parser.add_argument("--out", required=True, help="Path to generated C++ include")
    return parser.parse_args()


def list_stdlib_sources(src_root: Path) -> list[Path]:
    files: list[Path] = []
    for source in sorted(src_root.rglob("*.eta")):
        rel = source.relative_to(src_root)
        if rel.parts and rel.parts[0] == "tests":
            continue
        files.append(source)
    return files


def _strip_comment_prefix(line: str) -> str | None:
    stripped = line.lstrip()
    if not stripped.startswith(";;"):
        return None
    payload = stripped[2:]
    if payload.startswith(" "):
        payload = payload[1:]
    return payload.rstrip("\n")


def _is_comment_line(line: str) -> bool:
    return _strip_comment_prefix(line) is not None


def _detect_module_declarations(lines: list[str]) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    pattern = re.compile(r"^\s*\(\s*module\s+([^\s()]+)")
    for idx, line in enumerate(lines, start=1):
        if _is_comment_line(line):
            continue
        match = pattern.match(line)
        if match:
            out.append((idx, match.group(1)))
    return out


def _module_for_line(decls: list[tuple[int, str]], line: int) -> str:
    module_name = ""
    for decl_line, name in decls:
        if decl_line > line:
            break
        module_name = name
    return module_name


def _extract_form_head_and_symbol(line: str) -> tuple[str | None, str | None]:
    start = line.lstrip()
    if not start.startswith("("):
        return None, None

    head_match = re.match(r"^\(\s*([^\s()]+)(.*)$", start)
    if not head_match:
        return None, None
    head = head_match.group(1)
    rest = head_match.group(2)

    if head not in SUPPORTED_DOC_FORMS:
        return head, None

    if head == "defun":
        name_match = re.match(r"^\s*([^\s()]+)", rest)
        return head, name_match.group(1) if name_match else None

    if head in {"define-syntax", "defmacro", "define-macro"}:
        name_match = re.match(r"^\s*([^\s()]+)", rest)
        return head, name_match.group(1) if name_match else None

    if head == "define":
        rest = rest.lstrip()
        if rest.startswith("("):
            name_match = re.match(r"^\(\s*([^\s()]+)", rest)
            return head, name_match.group(1) if name_match else None
        name_match = re.match(r"^\s*([^\s()]+)", rest)
        return head, name_match.group(1) if name_match else None

    return head, None


def _paragraphs(lines: tuple[str, ...]) -> list[list[str]]:
    out: list[list[str]] = []
    current: list[str] = []
    for line in lines:
        if line.strip() == "":
            if current:
                out.append(current)
                current = []
            continue
        current.append(line.rstrip())
    if current:
        out.append(current)
    return out


def _build_summary_and_details(block: ParsedDocBlock) -> tuple[str, str]:
    paragraphs = _paragraphs(block.body_lines)

    summary = ""
    detail_parts: list[str] = []

    if paragraphs:
        summary = " ".join(chunk.strip() for chunk in paragraphs[0] if chunk.strip())
        for para in paragraphs[1:]:
            detail_parts.append("\n".join(para))

    if block.alias_of:
        detail_parts.append(f"Alias of `{block.alias_of}`.")
    if block.since:
        detail_parts.append(f"Since `{block.since}`.")
    if block.deprecated:
        detail_parts.append(f"Deprecated: {block.deprecated}")

    for example in block.examples:
        code = "\n".join(example).rstrip()
        if not code:
            continue
        detail_parts.append("**Example**\n\n```scheme\n" + code + "\n```")

    details = "\n\n".join(part for part in detail_parts if part.strip())
    return summary, details


def _parse_doc_block(file_path: Path,
                     block: list[tuple[int, str]]) -> tuple[ParsedDocBlock | None, list[Diagnostic]]:
    payloads = [(line_no, _strip_comment_prefix(text)) for line_no, text in block]
    payloads = [(line_no, payload if payload is not None else "") for line_no, payload in payloads]

    doc_lines = [idx for idx, (_, payload) in enumerate(payloads) if payload.startswith(TAG_DOC)]
    if not doc_lines:
        return None, []

    diagnostics: list[Diagnostic] = []
    if len(doc_lines) > 1:
        diagnostics.append(
            Diagnostic(
                file=file_path,
                line=payloads[doc_lines[1]][0],
                message="multiple ;;@doc tags found in one doc block",
            )
        )
        return None, diagnostics
    if doc_lines[0] != 0:
        diagnostics.append(
            Diagnostic(
                file=file_path,
                line=payloads[doc_lines[0]][0],
                message=";;@doc must be the first line of a doc block",
            )
        )
        return None, diagnostics

    doc_line_no, doc_payload = payloads[0]
    signature = doc_payload[len(TAG_DOC):].strip()
    if not signature:
        diagnostics.append(
            Diagnostic(file=file_path, line=doc_line_no, message=";;@doc requires a signature")
        )
        return None, diagnostics

    category = ""
    module = ""
    alias_of = ""
    since = ""
    deprecated = ""
    body_lines: list[str] = []
    examples: list[list[str]] = []
    current_example: list[str] | None = None

    def close_example() -> None:
        nonlocal current_example
        if current_example is not None:
            examples.append(current_example)
            current_example = None

    for line_no, payload in payloads[1:]:
        if payload.startswith(TAG_DOC):
            diagnostics.append(
                Diagnostic(file=file_path, line=line_no, message="multiple ;;@doc tags found in one doc block")
            )
            continue
        if payload.startswith(TAG_CATEGORY):
            close_example()
            if category:
                diagnostics.append(
                    Diagnostic(file=file_path, line=line_no, message="duplicate ;;@category tag")
                )
            category = payload[len(TAG_CATEGORY):].strip()
            continue
        if payload.startswith(TAG_MODULE):
            close_example()
            if module:
                diagnostics.append(
                    Diagnostic(file=file_path, line=line_no, message="duplicate ;;@module tag")
                )
            module = payload[len(TAG_MODULE):].strip()
            continue
        if payload.startswith(TAG_ALIAS_OF):
            close_example()
            if alias_of:
                diagnostics.append(
                    Diagnostic(file=file_path, line=line_no, message="duplicate ;;@alias-of tag")
                )
            alias_of = payload[len(TAG_ALIAS_OF):].strip()
            continue
        if payload.startswith(TAG_SINCE):
            close_example()
            if since:
                diagnostics.append(
                    Diagnostic(file=file_path, line=line_no, message="duplicate ;;@since tag")
                )
            since = payload[len(TAG_SINCE):].strip()
            continue
        if payload.startswith(TAG_DEPRECATED):
            close_example()
            if deprecated:
                diagnostics.append(
                    Diagnostic(file=file_path, line=line_no, message="duplicate ;;@deprecated tag")
                )
            deprecated = payload[len(TAG_DEPRECATED):].strip()
            continue
        if payload.strip() == TAG_EXAMPLE:
            close_example()
            current_example = []
            continue
        if payload.startswith("@"):
            diagnostics.append(
                Diagnostic(file=file_path, line=line_no, message=f"unsupported doc tag ';;{payload}'")
            )
            continue

        if current_example is not None:
            current_example.append(payload)
        else:
            body_lines.append(payload)

    close_example()

    if diagnostics:
        return None, diagnostics

    return ParsedDocBlock(
        line=doc_line_no,
        signature=signature,
        category=category,
        module=module,
        alias_of=alias_of,
        since=since,
        deprecated=deprecated,
        body_lines=tuple(body_lines),
        examples=tuple(tuple(example) for example in examples),
    ), []


def extract_docs_from_text(source: str, file_path: Path) -> tuple[list[ExtractedDoc], list[Diagnostic]]:
    lines = source.splitlines(keepends=True)
    module_decls = _detect_module_declarations(lines)

    docs: list[ExtractedDoc] = []
    diagnostics: list[Diagnostic] = []

    i = 0
    while i < len(lines):
        line = lines[i]
        if not _is_comment_line(line):
            i += 1
            continue

        block: list[tuple[int, str]] = []
        while i < len(lines) and _is_comment_line(lines[i]):
            block.append((i + 1, lines[i]))
            i += 1

        parsed, block_diags = _parse_doc_block(file_path, block)
        diagnostics.extend(block_diags)
        if parsed is None:
            continue

        if i >= len(lines):
            diagnostics.append(
                Diagnostic(
                    file=file_path,
                    line=parsed.line,
                    message="doc block must be followed immediately by a documented form",
                )
            )
            continue

        next_line = lines[i]
        if next_line.strip() == "":
            diagnostics.append(
                Diagnostic(
                    file=file_path,
                    line=parsed.line,
                    message="doc block must not be separated from the documented form by a blank line",
                )
            )
            continue

        head, symbol = _extract_form_head_and_symbol(next_line)
        if head is None:
            diagnostics.append(
                Diagnostic(
                    file=file_path,
                    line=i + 1,
                    message="doc block must be attached to a form starting with '('",
                )
            )
            continue
        if symbol is None:
            diagnostics.append(
                Diagnostic(
                    file=file_path,
                    line=i + 1,
                    message=f"unsupported or ambiguous documented form '({head} ...)'",
                )
            )
            continue

        module_name = parsed.module or _module_for_line(module_decls, i + 1)
        if not module_name:
            diagnostics.append(
                Diagnostic(
                    file=file_path,
                    line=i + 1,
                    message="cannot resolve module for documented symbol; add ;;@module",
                )
            )
            continue

        summary, details = _build_summary_and_details(parsed)
        qualified_name = f"{module_name}.{symbol}"
        docs.append(
            ExtractedDoc(
                file=file_path,
                line=i + 1,
                name=symbol,
                qualified_name=qualified_name,
                signature=parsed.signature,
                summary=summary,
                details=details,
                category=parsed.category,
                module=module_name,
                alias_of=parsed.alias_of,
                since=parsed.since,
                deprecated=parsed.deprecated,
                examples=parsed.examples,
            )
        )

    return docs, diagnostics


def extract_docs_from_file(file_path: Path) -> tuple[list[ExtractedDoc], list[Diagnostic]]:
    source = file_path.read_text(encoding="utf-8")
    return extract_docs_from_text(source, file_path)


def _check_duplicate_symbols(docs: list[ExtractedDoc]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    seen: dict[str, ExtractedDoc] = {}
    for doc in docs:
        key = doc.qualified_name
        if key not in seen:
            seen[key] = doc
            continue
        previous = seen[key]
        diagnostics.append(
            Diagnostic(
                file=doc.file,
                line=doc.line,
                message=(
                    f"duplicate stdlib doc symbol '{key}' "
                    f"(first seen at {previous.file}:{previous.line})"
                ),
            )
        )
    return diagnostics


def _cpp_quote(value: str) -> str:
    escaped = (
        value.replace("\\", "\\\\")
        .replace("\"", "\\\"")
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return f"\"{escaped}\""


def emit_registry_include(docs: list[ExtractedDoc], out_path: Path) -> None:
    ordered = sorted(docs, key=lambda d: (d.qualified_name, d.file.as_posix(), d.line))
    out_path.parent.mkdir(parents=True, exist_ok=True)

    lines: list[str] = []
    lines.append("// Generated by scripts/build_stdlib_docs.py. Do not edit manually.")
    lines.append("")

    if not ordered:
        lines.append("inline constexpr std::array<eta::docs::StdlibDocMetadata, 0> kStdlibDocs = {};")
        out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return

    lines.append(
        f"inline constexpr std::array<eta::docs::StdlibDocMetadata, {len(ordered)}> kStdlibDocs = {{"
    )
    lines.append("    {")
    for doc in ordered:
        lines.append(
            "        {"
            + ", ".join(
                [
                    _cpp_quote(doc.name),
                    _cpp_quote(doc.qualified_name),
                    _cpp_quote(doc.signature),
                    _cpp_quote(doc.summary),
                    _cpp_quote(doc.details),
                    _cpp_quote(doc.category),
                    _cpp_quote(doc.module),
                    _cpp_quote(doc.alias_of),
                    _cpp_quote(doc.since),
                    _cpp_quote(doc.deprecated),
                ]
            )
            + "},"
        )
    lines.append("    }")
    lines.append("};")

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run(src_root: Path, out_path: Path) -> int:
    if not src_root.is_dir():
        print(f"error: stdlib source root not found: {src_root}", file=sys.stderr)
        return 1

    sources = list_stdlib_sources(src_root)
    if not sources:
        print(f"error: no stdlib .eta files found under {src_root}", file=sys.stderr)
        return 1

    all_docs: list[ExtractedDoc] = []
    diagnostics: list[Diagnostic] = []

    for source in sources:
        docs, file_diagnostics = extract_docs_from_file(source)
        all_docs.extend(docs)
        diagnostics.extend(file_diagnostics)

    diagnostics.extend(_check_duplicate_symbols(all_docs))
    if diagnostics:
        for diag in diagnostics:
            print(diag.format(), file=sys.stderr)
        return 1

    emit_registry_include(all_docs, out_path)
    print(f"generated stdlib doc registry: {out_path} ({len(all_docs)} entries)")
    return 0


def main() -> int:
    args = parse_args()
    src_root = Path(args.src_root).resolve()
    out_path = Path(args.out).resolve()
    return run(src_root, out_path)


if __name__ == "__main__":
    raise SystemExit(main())
