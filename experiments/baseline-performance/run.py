"""Same-machine parse + free comparison; see README.md for scope and commands."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "experiments/incremental"))
from run import Core, Document, POINTER, SIZE


class Native:
    def __init__(self, path):
        self.lib = C.CDLL(str(path))
        self.path = str(path)
        for name, args, result in (
            ("markdown_core_document_parse", [C.c_char_p, SIZE, POINTER], POINTER),
            ("markdown_core_document_free", [POINTER], None),
            ("markdown_core_document_dump", [POINTER, C.POINTER(POINTER), C.POINTER(SIZE), POINTER], C.c_bool),
            ("markdown_core_dump_free", [POINTER], None),
        ):
            fn = getattr(self.lib, name)
            fn.argtypes, fn.restype = args, result

    def sample(self, source):
        start = time.perf_counter_ns()
        doc = Document(self, source)
        doc.close()
        return time.perf_counter_ns() - start

    def dump(self, source):
        doc = Document(self, source)
        try:
            return doc.dump()
        finally:
            doc.close()


def main():
    arg = argparse.ArgumentParser()
    arg.add_argument("--baseline-library", type=Path, required=True)
    arg.add_argument("--candidate-build", type=Path, default=ROOT / "build/benchmark")
    arg.add_argument("--samples", type=int, default=7)
    arg.add_argument("--output", type=Path, required=True)
    options = arg.parse_args()
    if options.samples < 1:
        arg.error("samples must be positive")
    core = Core(options.candidate_build.resolve())
    baseline = Native(options.baseline_library.resolve())
    candidate = Native(Path(core.lib._name))
    assert C.cast(baseline.lib.markdown_core_document_parse, POINTER).value != C.cast(
        candidate.lib.markdown_core_document_parse, POINTER).value, "libraries must be distinct"
    cases = {}
    for depth in (1024, 2048, 4096):
        cases[f"nested_list_continuation_{depth}"] = b"- " * depth + b"leaf\n" + b"  " * depth + b"more\n"
        cases[f"nested_quote_continuation_{depth}"] = b"> " * depth + b"leaf\n" + b"> " * depth + b"more\n"
    for n in (256, 512, 1024, 2048):
        keys, generated, branches = C.create_string_buffer(n * 18), SIZE(), SIZE()
        assert core.probe.mc_collisions(n, keys, C.byref(generated), C.byref(branches))
        labels = [keys.raw[i * 18:i * 18 + 17] for i in range(n)]
        cases[f"reference_flood_{n}"] = b"".join(b"[" + k + b"]: /u\n" for k in labels) + b"\n" + b"".join(
            b"[" + k + b"]\n\n" for k in labels)
        cases[f"heading_flood_{n}"] = b"".join(b"# " + k + b"\n\n" for k in labels)
        cases[f"footnote_flood_{n}"] = b"".join(b"[^" + k + b"]: body\n\n" for k in labels) + b"".join(
            b"[^" + k + b"]\n\n" for k in labels)
        cases[f"specimen_flood_{n}"] = b"".join(b"(@" + k + b") body\n\n" for k in labels) + b"".join(
            b"@" + k + b"\n\n" for k in labels)
    cases["prose_long_paragraph"] = b"This is a paragraph about parsing ordinary prose and its semantic boundaries.\n" * 2000
    cases["text_without_whitespace"] = b"a" * 1048576
    cases["unicode_prose"] = ("这是包含 Unicode 空白的文本\u2003以及其他文字。\n" * 2000).encode()
    cases["emphasis_long_paragraph"] = b"a *b* c **d** e _f_ g\n" * 2000
    for sample in sorted((ROOT / "packages/markdown-core/benchmarks/samples").glob("*.md")):
        cases[f"tracked/{sample.name}"] = sample.read_bytes()
    results = {}
    for name, source in cases.items():
        # Canonical output would itself be quadratic for the deep tree. Its
        # semantics are covered by the native depth tests instead.
        if not name.startswith("nested_"):
            assert baseline.dump(source) == candidate.dump(source), name
        baseline.sample(source)
        candidate.sample(source)
        timings = [[], []]
        for repetition in range(options.samples):
            # Alternate order to reduce systematic warm-cache/order bias.
            for lane in ((0, 1) if repetition % 2 == 0 else (1, 0)):
                timings[lane].append((baseline, candidate)[lane].sample(source))
        medians = [statistics.median(values) for values in timings]
        results[name] = {
            "bytes": len(source), "source_sha256": hashlib.sha256(source).hexdigest(),
            "baseline_samples_ns": timings[0], "candidate_samples_ns": timings[1],
            "baseline_median_ns": medians[0], "candidate_median_ns": medians[1],
            "speedup": medians[0] / medians[1],
            "candidate_MiB_per_second": len(source) * 1e9 / medians[1] / 1048576,
            "canonical_equivalent": None if name.startswith("nested_") else True,
        }
    output = {
        "baseline_revision": "5eca3bc1598b0313a0ac6a9e69f078a309feb3e1",
        "candidate": "working tree: radix index, separator rejection, last whitespace boundary",
        "platform": platform.platform(), "machine": platform.machine(),
        "compiler": subprocess.check_output(["cc", "--version"], text=True).splitlines()[0],
        "clock": "perf_counter_ns", "metric": "parse + free, no dump/instrumentation",
        "samples": options.samples, "warmups": 1,
        "baseline_library_sha256": hashlib.sha256(Path(baseline.path).read_bytes()).hexdigest(),
        "candidate_library_sha256": hashlib.sha256(Path(candidate.path).read_bytes()).hexdigest(),
        "cases": results,
    }
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(output, indent=4) + "\n")
    print(options.output)


if __name__ == "__main__":
    main()
