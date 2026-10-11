"""Freeze/verify the Phase-0 corpus and record reproducible reader timings."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import platform
import random
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs/benchmarks/tooling_manifest.json"
REPORT = ROOT / "docs/benchmarks/tooling_baseline.json"
SEED = 0xE7A0


def generated():
    rng = random.Random(SEED)
    comment = ";" + "".join(rng.choice("abcdefghijklmnopqrstuvwxyz ") for _ in range(1024 * 1024 - 2)) + "\n"
    return {"comment-heavy-1m": comment.encode(), "depth-256": ("(" * 256 + "x" + ")" * 256).encode(),
            "ordinary-medium": ("(define x '(1 2 3))\n" * 1000).encode(),
            "ordinary-large": ("(define x '(1 2 3))\n" * 8000).encode()}


def bin_for(size):
    return "small" if size <= 4096 else "medium" if size <= 65536 else "large"


def corpus():
    result = []
    for directory in ("stdlib", "cookbook", "demo"):
        for path in sorted((ROOT / directory).rglob("*.eta")):
            data = path.read_bytes()
            result.append({"path": path.relative_to(ROOT).as_posix(), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "bin": bin_for(len(data))})
    for name, data in generated().items():
        result.append({"generated": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "bin": bin_for(len(data))})
    return result


def manifest():
    files = corpus()
    selection = []
    # Stable representative files from each corpus and size bin, then all
    # generated cases. The preservation gate covers the entire manifest.
    for directory in ("stdlib", "cookbook", "demo"):
        for size in ("small", "medium", "large"):
            group = [item for item in files if item.get("path", "").startswith(directory + "/") and item["bin"] == size]
            if group:
                selection.append(group[len(group) // 2]["path"])
    selection += list(generated())
    return {"schema": 1, "corpus_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            "corpora": {name: sum(item.get("path", "").startswith(name + "/") for item in files) for name in ("stdlib", "cookbook", "demo")},
            "generator_seed": SEED, "generator": "scripts/tooling_benchmark.py:generated", "files": files, "measurement_selection": selection,
            "options": {"jobs": 1, "strict_quasiquote": False, "grammar": "EtaCurrent", "preset": None,
                        "input_bytes": 16777216, "output_bytes": 33554432, "depth": 256, "tokens": 1000000, "nodes": 1000000, "memory_bytes": 268435456},
            "method": {"warmup": 10, "measured": 100, "timer": "std::chrono::steady_clock",
                       "median": "mean of sorted samples 49 and 50 (zero based)", "p95": "nearest rank, sample 94 (zero based)",
                       "library": "resident input; single worker; allocation/destruction included; CST includes reader validation",
                       "process_io": "fresh --probe process startup/read/parse measured separately; warm I/O uses 10 warmup/100 resident-process rereads; page cache uncontrolled",
                       "memory": "process peak RSS; tooling arena peak plus conservative reader/transient reservations; shared runtime excluded from owned memory",
                       "cancellation": "10 warmup + 100 measured 16 MiB comments; cancellation request after 2 ms; latency measured from request to parse return"},
            "initial_budgets": {"formatter_small_p95_ms": 2, "per_rule_small_p95_ms": 1, "cancellation_p95_ms": 50}}


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")


def record(binary, build):
    frozen = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if frozen["files"] != corpus():
        raise SystemExit("Corpus hashes differ from the frozen manifest")
    machine = {"cpu": platform.processor(), "logical_cores": __import__("os").cpu_count(), "os": platform.platform(), "architecture": platform.machine()}
    if platform.system() == "Windows":
        command = "$cpu=Get-CimInstance Win32_Processor; $os=Get-CimInstance Win32_ComputerSystem; @{cpu=$cpu.Name; physical_cores=$cpu.NumberOfCores; ram_bytes=$os.TotalPhysicalMemory} | ConvertTo-Json -Compress"
        machine.update(json.loads(subprocess.check_output(["powershell", "-NoProfile", "-Command", command], text=True)))
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    settings = {line.split(":", 1)[0]: line.split("=", 1)[1] for line in cache.splitlines()
                if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS", "CMAKE_GENERATOR:", "ETA_ENABLE_PACKAGE_NATIVE_SIDECARS:"))}
    compiler = subprocess.run([settings["CMAKE_CXX_COMPILER"]], capture_output=True, text=True)
    settings["compiler_version"] = (compiler.stderr + compiler.stdout).splitlines()[0]
    samples = []
    with tempfile.TemporaryDirectory(prefix="eta-tooling-benchmark-") as directory:
        for name, data in generated().items():
            (pathlib.Path(directory) / name).write_bytes(data)
        for name in frozen["measurement_selection"]:
            path = ROOT / name if "/" in name else pathlib.Path(directory) / name
            probe_start = time.perf_counter()
            probe = json.loads(subprocess.check_output([str(binary.resolve()), "--probe", str(path)], text=True))
            cold_process_ms = (time.perf_counter() - probe_start) * 1000
            start = time.perf_counter()
            output = subprocess.check_output([str(binary.resolve()), str(path)], text=True)
            elapsed = (time.perf_counter() - start) * 1000
            sample = {"input": name, "bin": bin_for(path.stat().st_size), "cold_process_startup_io_parse_ms": cold_process_ms,
                      "first_io_ms": probe["first_io_ms"], "measurement_process_total_ms": elapsed, **json.loads(output)}
            samples.append(sample)
            print(f"{name}: reader p95 {sample['reader']['p95_ms']:.3f} ms; CST p95 {sample['cst_with_validation']['p95_ms']:.3f} ms", flush=True)
    bins = {}
    for name in ("small", "medium", "large"):
        group = [sample for sample in samples if sample["bin"] == name]
        bins[name] = {"files": len(group), "reader_file_p95_max_ms": max((item["reader"]["p95_ms"] for item in group), default=None),
                      "cst_file_p95_max_ms": max((item["cst_with_validation"]["p95_ms"] for item in group), default=None)}
    save(REPORT, {"manifest_sha256": hashlib.sha256(MANIFEST.read_bytes()).hexdigest(), "reference_machine": machine,
                  "build": settings, "assertions": "disabled (NDEBUG)", "sanitizers": "none in MSVC Release reference build",
                  "samples": samples, "bins": bins,
                  "cancellation_p95_max_ms": max(item["cancellation_p95_ms"] for item in samples)})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--freeze", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--record", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, default=ROOT / "out/msvc-release")
    args = parser.parse_args()
    if args.freeze:
        save(MANIFEST, manifest())
    if args.check:
        frozen = json.loads(MANIFEST.read_text(encoding="utf-8"))
        if frozen["files"] != corpus():
            raise SystemExit("Tooling benchmark corpus/hash manifest is stale; refresh explicitly for intentional corpus changes")
        print(f"Verified {len(frozen['files'])} corpus entries and deterministic generators")
    if args.record:
        record(args.record, args.build)


if __name__ == "__main__":
    main()
