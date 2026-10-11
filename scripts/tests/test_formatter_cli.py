"""Formatter CLI contract and real-process corpus idempotence tests."""

from __future__ import annotations

import pathlib
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

BINARY, INTERPRETER, ROOT = map(pathlib.Path, sys.argv[1:4])
sys.argv = sys.argv[:1]


class FormatterCli(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="eta-fmt-cli-")
        self.directory = pathlib.Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)

    def run_fmt(self, *args, data=None, cwd=None):
        return subprocess.run([str(BINARY), *map(str, args)], input=data, capture_output=True,
                              cwd=cwd or self.directory, timeout=60)

    def file(self, name="test.eta", source=b"(h   a)\n"):
        path = self.directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(source)
        return path

    def test_usage_preflight(self):
        path = self.file()
        for args in (("--check", "--diff", path), ("--stdout",), ("--stdout", self.directory),
                     ("--stdout", path, path), ("--indent", "0", path), ("--jobs", "-1", path),
                     ("--max-width", "x", path), ("--config", "x", path), ("--stdin", path),
                     ("--stdin-filename", "x"), ("--stdin", "--stdout")):
            with self.subTest(args=args):
                result = self.run_fmt(*args)
                self.assertEqual(result.returncode, 64, result.stderr)
                self.assertEqual(result.stdout, b"")
                self.assertEqual(path.read_bytes(), b"(h   a)\n")
        self.assertEqual(self.run_fmt("--help").returncode, 0)
        self.assertIn(b"eta_fmt", self.run_fmt("--version").stdout)

    def test_stdin_stdout_check_diff_and_failures(self):
        result = self.run_fmt("--stdin", "--stdin-filename", "logical.eta", data=b"(h   a)\r\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"(h a)\r\n")
        self.assertFalse((self.directory / "logical.eta").exists())
        result = self.run_fmt("--stdin", "--check", data=b"(h   a)")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")
        result = self.run_fmt("--stdin", "--diff", "--stdin-filename", "logical.eta", data=b"(h   a)")
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"--- a/logical.eta", result.stdout)
        self.assertIn(b"\\ No newline", result.stdout)
        for data in (b"(h", b"(h)\r\n(a)\n", b";\xff\nx", b"\xef\xbb\xbf(h)"):
            result = self.run_fmt("--stdin", data=data)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertEqual(result.stdout, b"")
        path = self.file()
        result = self.run_fmt("--stdout", path)
        self.assertEqual(result.stdout, b"(h a)\n")
        self.assertEqual(path.read_bytes(), b"(h   a)\n")

    def test_discovery_order_exclusions_duplicates_and_aggregation(self):
        a = self.file("a.eta")
        b = self.file("nested/b.eta")
        invalid = self.file("bad.eta", b"(h")
        self.file("out/ignored.eta", b"(h")
        self.file("build/ignored.eta", b"(h")
        self.file(".eta/ignored.eta", b"(h")
        text = self.file("explicit.txt")
        result = self.run_fmt("--diff", "--jobs", "4", a, b, a)
        serial = self.run_fmt("--diff", "--jobs", "1", b, a)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, serial.stdout)
        self.assertEqual(result.stdout.count(b"--- a/a.eta"), 1)
        self.assertEqual(self.run_fmt("--check", "--exclude", "bad.eta", ".").returncode, 1)
        result = self.run_fmt("--diff", ".")
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"--- a/a.eta", result.stdout)
        self.assertNotIn(b"--- a/bad.eta", result.stdout)
        result = self.run_fmt(a, invalid, text)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(a.read_bytes(), b"(h a)\n")
        self.assertEqual(invalid.read_bytes(), b"(h")
        self.assertEqual(text.read_bytes(), b"(h a)\n")
        self.assertEqual(self.run_fmt("--check", "--exclude", "**", ".").returncode, 0)
        self.assertEqual(self.run_fmt("--check", "missing.eta").returncode, 2)

    def test_readonly_and_hardlinks(self):
        path = self.file()
        path.chmod(stat.S_IREAD)
        try:
            result = self.run_fmt(path)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertEqual(path.read_bytes(), b"(h   a)\n")
        finally:
            path.chmod(stat.S_IREAD | stat.S_IWRITE)
        self.assertEqual(self.run_fmt(path).returncode, 0)
        path.chmod(stat.S_IREAD)
        try:
            before = path.stat()
            self.assertEqual(self.run_fmt(path).returncode, 0)
            self.assertEqual(path.stat().st_mtime_ns, before.st_mtime_ns)
        finally:
            path.chmod(stat.S_IREAD | stat.S_IWRITE)
        alias = self.directory / "alias.eta"
        alias.hardlink_to(path)
        self.assertEqual(self.run_fmt(path).returncode, 2)
        self.assertEqual(self.run_fmt("--check", path, alias).returncode, 0)
        try:
            link = self.directory / "link.eta"
            link.symlink_to(path)
        except OSError:
            return  # Windows without Developer Mode cannot create symbolic links.
        self.assertEqual(self.run_fmt(link).returncode, 2)
        self.assertEqual(self.run_fmt("--stdout", link).returncode, 0)

    def test_corpus_cli_twice_and_check(self):
        paths = []
        for name in ("stdlib", "cookbook", "demo"):
            for source in sorted((ROOT / name).rglob("*.eta")):
                target = self.directory / source.relative_to(ROOT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
                paths.append(target)
        self.assertGreater(len(paths), 100)
        refused = self.directory / "stdlib/std/test.eta"
        original_refusal = (refused.read_bytes(), refused.stat().st_mtime_ns, refused.stat().st_ctime_ns)
        result = self.run_fmt("--jobs", "4", ".")
        self.assertEqual(result.returncode, 2, result.stderr.decode(errors="replace"))
        self.assertIn(b"stdlib/std/test.eta: error[format.LineEndings]", result.stderr)
        self.assertEqual(len([line for line in result.stderr.splitlines() if b": error" in line]), 1)
        reviewed = json.loads((ROOT / "docs/benchmarks/formatter_coverage.json").read_text(encoding="utf-8"))
        self.assertEqual(reviewed["reviewed_refusals"][0]["sha256"], hashlib.sha256(refused.read_bytes()).hexdigest())
        self.assertEqual(original_refusal, (refused.read_bytes(), refused.stat().st_mtime_ns, refused.stat().st_ctime_ns))
        before = {path: (path.read_bytes(), path.stat().st_mtime_ns, path.stat().st_ctime_ns) for path in paths}
        result = self.run_fmt("--jobs", "4", ".")
        self.assertEqual(result.returncode, 2, result.stderr.decode(errors="replace"))
        self.assertEqual(before, {path: (path.read_bytes(), path.stat().st_mtime_ns, path.stat().st_ctime_ns) for path in paths})
        result = self.run_fmt("--check", "--exclude", "stdlib/std/test.eta", "--jobs", "4", ".")
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_unicode_paths_and_installed_layout(self):
        path = self.file("unicode-\u03bb.eta")
        result = self.run_fmt("--stdout", path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"(h a)\n")
        isolated = self.directory / "isolated-bin"
        isolated.mkdir()
        binary = isolated / BINARY.name
        shutil.copyfile(BINARY, binary)
        for dependency in BINARY.parent.glob("*.dll"):
            shutil.copyfile(dependency, isolated / dependency.name)
        environment = dict(os.environ)
        if os.name == "nt":
            environment["PATH"] = str(isolated) + os.pathsep + str(pathlib.Path(os.environ["SystemRoot"]) / "System32")
        else:
            environment.pop("LD_LIBRARY_PATH", None)
        result = subprocess.run([str(binary), "--stdin"], input=b"(h   a)", capture_output=True,
                                cwd=self.directory, env=environment, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"(h a)")

    def test_controlled_process_semantics(self):
        program = b"""(module fmt-output (import std.io) (begin
          (define-syntax twice (syntax-rules () ((twice x) (+ x x))))
          (println (twice 6)) (println '#(a b))
          (println (let ((x 4)) (if (= x 4) 'yes 'no)))))\n"""
        original = self.file("original.eta", program)
        formatted = self.run_fmt("--stdout", "--max-width", "20", original)
        self.assertEqual(formatted.returncode, 0, formatted.stderr)
        copy = self.file("formatted.eta", formatted.stdout)
        environment = dict(os.environ, ETA_MODULE_PATH=str(ROOT / "stdlib"))
        runs = [subprocess.run([str(INTERPRETER), str(path)], capture_output=True, env=environment,
                               cwd=self.directory, timeout=20) for path in (original, copy)]
        self.assertEqual(runs[0].returncode, 0, runs[0].stderr)
        self.assertEqual((runs[0].returncode, runs[0].stdout, runs[0].stderr),
                         (runs[1].returncode, runs[1].stdout, runs[1].stderr))
        self.assertIn(b"12", runs[0].stdout)

    @unittest.skipUnless(os.name == "nt", "NTFS metadata validation")
    def test_named_stream_and_creation_metadata(self):
        path = self.file()
        stream = pathlib.Path(str(path) + ":eta-test-metadata")
        stream.write_bytes(b"preserved named stream")
        before = path.stat().st_birthtime_ns
        result = self.run_fmt(path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(stream.read_bytes(), b"preserved named stream")
        self.assertEqual(path.stat().st_birthtime_ns, before)


if __name__ == "__main__":
    unittest.main()
