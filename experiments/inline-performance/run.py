"""Compare source-map, text-dispatch and attribute recognition changes end to end."""
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
import run as probe


def cases():
    result = {}
    for count in (1000, 2000, 4000):
        for name, unit in {
            "emphasis": b"a *b* c **d** e _f_ g\n",
            "prose": b"the world is here and now we know\n",
            "bang": b"a! b! c! d! e! f! g! h! i! j! k! l! m!\n",
            "colon": b"a: b: c: d: e: f: g: h: i: j: k: l: m:\n",
            "tilde": b"a ~ b ~ c ~ d ~ e ~ f ~ g ~ h ~ i ~ j\n",
        }.items():
            result[f"{name}/{count}"] = unit * count
    for size in (16384, 65536, 262144):
        plain = (b"ordinary text in a paragraph " * (size // 29 + 1))[:size]
        for name, source in {
            "none": plain,
            "sparse_end": plain + b" [x]{.c}",
            "failed_end": plain + b" [x]{",
            "sparse_front": b"[x]{.c} " + plain,
            "failed_front": b"[x]{?} " + plain,
            "dense": b"text [x]{.c} abc " * (size // 17),
            "dense_failed": b"text [x]{b c abc " * (size // 17),
            "reference_and_span": b"[r]: /u {.c}\n" + plain + b" [x]{.d}",
        }.items():
            result[f"attributes/{name}/{size}"] = source
    for name, unit in {
        "headings": b"# title {.c}\n\n",
        "directives": b"::note{.c k=1}\n\n",
        "references": b"[r]: /u {.c}\n",
    }.items():
        result[f"attributes/{name}"] = unit * 2000
    result["ci/representative_large_v1"] = (
        "## Section\n\nParagraph with **strong**, [link](https://example.com), and 🚀.\n\n".encode() * 2000
    )
    for path in sorted((ROOT / "packages/markdown-core/benchmarks/samples").glob("*.md")):
        result[f"tracked/{path.name}"] = path.read_bytes()
    return result


def measure_stats(core, source):
    stats = probe.Stats()
    assert core.probe.mc_probe(source, len(source), C.byref(stats)), "parse failed or leaked"
    return {name: getattr(stats, name) for name, _ in stats._fields_}


def sample(core, source, iterations=1):
    start = time.perf_counter_ns()
    for _ in range(iterations):
        core.parse(source).close()
    return (time.perf_counter_ns() - start) / iterations


def main():
    arg = argparse.ArgumentParser(description=__doc__)
    arg.add_argument("--baseline-source", type=Path, required=True)
    arg.add_argument("--baseline-revision", required=True)
    arg.add_argument("--candidate-build", type=Path, default=ROOT / "build/benchmark")
    arg.add_argument("--samples", type=int, default=7)
    arg.add_argument("--output", type=Path, required=True)
    args = arg.parse_args()
    if args.samples < 1:
        arg.error("samples must be positive")
    probe.ROOT = args.baseline_source.resolve()
    baseline = probe.Core(probe.ROOT / "build/benchmark")
    probe.ROOT = ROOT
    candidate = probe.Core(args.candidate_build.resolve())
    assert baseline.lib._name != candidate.lib._name
    rows = {}
    for name, source in cases().items():
        assert baseline.dump(source) == candidate.dump(source), name
        warmup = max(sample(baseline, source), sample(candidate, source))
        # Calibrate measurement duration, never parser behavior. Batches reduce
        # clock/scheduling noise for the original very small tracked samples.
        iterations = max(1, min(2000, int(2_000_000 / warmup)))
        times = [[], []]
        for repeat in range(args.samples):
            for lane in ((0, 1) if repeat % 2 == 0 else (1, 0)):
                times[lane].append(sample((baseline, candidate)[lane], source, iterations))
        medians = [int(statistics.median(t)) for t in times]
        rows[name] = {
            "bytes": len(source), "source_sha256": hashlib.sha256(source).hexdigest(),
            "canonical_equivalent": True, "batch_iterations": iterations, "baseline_samples_ns": times[0], "candidate_samples_ns": times[1],
            "speedup": medians[0] / medians[1],
            "baseline": {"parse_free_median_ns": medians[0], **measure_stats(baseline, source)},
            "candidate": {"parse_free_median_ns": medians[1], **measure_stats(candidate, source)},
        }
    output = {
        "baseline_revision": args.baseline_revision,
        "candidate_base": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "candidate_changes": "content projection, inline candidates, sparse forward attribute facts",
        "platform": platform.platform(), "machine": platform.machine(), "samples": args.samples,
        "compiler": subprocess.check_output(["cc", "--version"], text=True).splitlines()[0],
        "metric": "complete C parse + free via identical ctypes calls; calibrated batches and alternating lanes; dumps and allocation instrumentation excluded",
        "baseline_library_sha256": hashlib.sha256(Path(baseline.lib._name).read_bytes()).hexdigest(),
        "candidate_library_sha256": hashlib.sha256(Path(candidate.lib._name).read_bytes()).hexdigest(),
        "cases": rows,
    }
    args.output.write_text(json.dumps(output, indent=4) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
