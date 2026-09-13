"""Isolate the first batch's map change using the unchanged CI parse-only runner."""
import argparse
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import platform
import re
import statistics
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASE = "5eca3bc1598b0313a0ac6a9e69f078a309feb3e1"
HEAD = "84efce3220e6e88fee1e6d5954b88eca82960924"
VARIANTS = {
    "base": (BASE, BASE),
    "map-only": (BASE, HEAD),
    "other-fixes": (HEAD, BASE),
    "first-batch": (HEAD, HEAD),
}


def main():
    arg = argparse.ArgumentParser(description=__doc__)
    arg.add_argument("--output", type=Path, required=True)
    arg.add_argument("--cc", default="clang")
    arg.add_argument("--rounds", type=int, default=16)
    args = arg.parse_args()
    if args.rounds < 1:
        arg.error("rounds must be positive")
    spec = importlib.util.spec_from_file_location("baseline_compare", Path(__file__).with_name("run.py"))
    compare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(compare)
    source = "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n".encode() * 2000
    records, libraries, commands = {}, {}, {}
    with tempfile.TemporaryDirectory(prefix="markdown-core-map-attribution-") as directory:
        for name, (revision, map_revision) in VARIANTS.items():
            target = Path(directory) / name
            target.mkdir()
            archive = subprocess.check_output(["git", "archive", revision], cwd=ROOT)
            with tarfile.open(fileobj=io.BytesIO(archive)) as contents:
                contents.extractall(target, filter="data")
            for path in ("packages/markdown-core/core/map.c", "packages/markdown-core/core/map.h"):
                (target / path).write_bytes(subprocess.check_output(["git", "show", f"{map_revision}:{path}"], cwd=ROOT))
            build = target / "build/benchmark"
            subprocess.run(["cmake", "-S", str(target), "-B", str(build),
                            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={args.cc}",
                            "-DMARKDOWN_CORE_BENCHMARKS=ON"], check=True, capture_output=True)
            subprocess.run(["cmake", "--build", str(build), "--target", "bench_runner",
                            "libmarkdown-core-elements", "--parallel", "2"], check=True, capture_output=True)
            products = build / "packages/markdown-core/elements"
            library = next(path for path in products.iterdir() if not path.is_symlink() and
                           (path.name.endswith(".dylib") or ".so" in path.name))
            libraries[name] = compare.Native(library)
            commands[name] = [str(build / "packages/markdown-core/tests/bench_runner"),
                              "--workload", "binding_baseline", "--samples",
                              str(target / "packages/markdown-core/benchmarks/samples"),
                              "--warmup", "2", "--repeats", "9"]
            records[name] = {"source_revision": revision, "map_revision": map_revision,
                             "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
                             "process_medians_ns": []}
        assert len({core.dump(source) for core in libraries.values()}) == 1, "canonical output differs"
        names = list(VARIANTS)
        for repeat in range(args.rounds):
            order = names[repeat % len(names):] + names[:repeat % len(names)]
            if repeat % 2:
                order.reverse()
            for name in order:
                output = subprocess.check_output(commands[name], text=True)
                records[name]["process_medians_ns"].append(int(re.search(r"median_ns=(\d+)", output)[1]))
    for record in records.values():
        record["median_ns"] = statistics.median(record["process_medians_ns"])
    result = {
        "workload": "representative_large v1", "bytes": len(source),
        "platform": platform.platform(),
        "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
        "metric": "unchanged CI bench_runner: parse only; rotating process order; median of process medians",
        "rounds": args.rounds, "warmup_per_process": 2, "repeats_per_process": 9,
        "source_sha256": hashlib.sha256(source).hexdigest(), "canonical_equivalent": True,
        "variants": records,
    }
    args.output.write_text(json.dumps(result, indent=4) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
