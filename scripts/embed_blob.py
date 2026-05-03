#!/usr/bin/env python3
"""Embed a binary blob into a C++ translation unit."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--in", dest="input_file", required=True, help="Input binary file")
    parser.add_argument("--out", dest="output_file", required=True, help="Output C++ file")
    parser.add_argument("--sym", dest="symbol", required=True, help="C++ symbol base name")
    return parser.parse_args()


def format_bytes(data: bytes) -> str:
    if not data:
        return "    0x00"

    lines: list[str] = []
    row: list[str] = []
    for idx, byte in enumerate(data):
        row.append(f"0x{byte:02x}")
        if (idx + 1) % 16 == 0:
            lines.append("    " + ", ".join(row))
            row = []
    if row:
        lines.append("    " + ", ".join(row))
    return ",\n".join(lines)


def main() -> int:
    args = parse_args()
    input_file = Path(args.input_file).resolve()
    output_file = Path(args.output_file).resolve()

    data = input_file.read_bytes()
    output_file.parent.mkdir(parents=True, exist_ok=True)

    array_data = format_bytes(data)
    output = (
        "#include <cstddef>\n"
        "#include <cstdint>\n\n"
        "namespace eta::runtime {\n\n"
        f"extern const std::uint8_t {args.symbol}[] = {{\n"
        f"{array_data}\n"
        "};\n\n"
        f"extern const std::size_t {args.symbol}_size = sizeof({args.symbol});\n\n"
        "} ///< namespace eta::runtime\n"
    )
    output_file.write_text(output, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
