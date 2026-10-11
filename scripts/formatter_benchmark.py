"""Record the Phase 1 formatter baseline against the frozen Phase 0 corpus."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile
import time

from tooling_benchmark import MANIFEST, ROOT, corpus, generated, save


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    args = parser.parse_args()
    frozen = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if frozen["files"] != corpus():
        raise SystemExit("Frozen corpus has changed")
    reference = json.loads((ROOT / "docs/benchmarks/tooling_baseline.json").read_text(encoding="utf-8"))
    samples = []
    with tempfile.TemporaryDirectory(prefix="eta-formatter-benchmark-") as directory:
        for name, data in generated().items():
            (pathlib.Path(directory) / name).write_bytes(data)
        for name in frozen["measurement_selection"]:
            path = ROOT / name if "/" in name else pathlib.Path(directory) / name
            start = time.perf_counter()
            probe = json.loads(subprocess.check_output([str(args.binary.resolve()), "--probe", str(path)], text=True))
            cold_ms = (time.perf_counter() - start) * 1000
            data = json.loads(subprocess.check_output([str(args.binary.resolve()), str(path)], text=True))
            item = next(item for item in frozen["files"] if item.get("path", item.get("generated")) == name)
            samples.append({"input": name, "bin": item["bin"], "cold_process_startup_io_format_ms": cold_ms, **probe, **data})
            print(f"{name}: format p95 {data['format_with_validation']['p95_ms']:.3f} ms", flush=True)
    bins = {name: {"files": sum(item["bin"] == name for item in samples),
                   "format_file_p95_max_ms": max(item["format_with_validation"]["p95_ms"] for item in samples if item["bin"] == name)}
            for name in ("small", "medium", "large")}
    save(ROOT / "docs/benchmarks/formatter_baseline.json", {
        "schema": 1, "manifest_sha256": hashlib.sha256(MANIFEST.read_bytes()).hexdigest(),
        "reference_machine": reference["reference_machine"], "build": reference["build"],
        "assertions": reference["assertions"], "sanitizers": reference["sanitizers"],
        "options": {"width": 100, "indent": 2, "reader": "EtaCurrent/non-strict", "newline": "captured per input", "jobs": 1},
        "method": {**frozen["method"], "library": "resident source, one worker, allocation/destruction included; total includes input/output reader and preservation validation",
                   "breakdown": "parse includes newline scan and CST/reader validation; layout_render includes lossless serialization and Doc construction/render; output_validation includes fresh output CST plus fresh original compiler readers",
                   "process_io": "fresh --probe startup/read/format separately; page cache uncontrolled",
                   "memory": "process peak RSS plus conservatively accounted peak for both CST snapshots, Doc arenas, output/reader reservations",
                   "cancellation": "10 warmup/100 measured 16 MiB comments, request after 2 ms; actual request-to-return latency"},
        "samples": samples, "bins": bins, "cancellation_p95_max_ms": max(item["cancellation_p95_ms"] for item in samples),
        "initial_small_target_ms": 2, "release_budget_phase": 5})


if __name__ == "__main__":
    main()
