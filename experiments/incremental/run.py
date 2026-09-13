"""Reproducible review and deliberately restricted incremental experiment.

Python is orchestration/source storage; every Markdown parse uses the selected
full C parser. No timing includes oracle dumps. See README.md for excluded costs.
"""

import argparse
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
POINTER = C.c_void_p
SIZE = C.c_size_t


class Stats(C.Structure):
    _fields_ = [(name, SIZE) for name in (
        "allocations", "requested_bytes", "live_bytes", "peak_bytes",
        "attributes", "lookahead", "tables", "anchors", "delimiters", "brackets",
        "node_size", "parser_size",
    )]


class Document:
    def __init__(self, core, source):
        self.core = core
        self.pointer = core.lib.markdown_core_document_parse(source, len(source), None)
        if not self.pointer:
            raise MemoryError("baseline parse failed")

    def close(self):
        if self.pointer:
            self.core.lib.markdown_core_document_free(self.pointer)
            self.pointer = None

    def dump(self):
        output, length = POINTER(), SIZE()
        if not self.core.lib.markdown_core_document_dump(self.pointer, C.byref(output), C.byref(length), None):
            raise MemoryError("baseline dump failed")
        try:
            return C.string_at(output, length.value).decode("utf-8")
        finally:
            self.core.lib.markdown_core_dump_free(output)


def prepare_probe(build):
    """Resolve both the adapter and private headers from the selected build."""
    cache = dict(re.findall(r"^([^#/:=\n]+):[^=\n]+=(.*)$", (build / "CMakeCache.txt").read_text(), re.MULTILINE))
    source = Path(cache["CMAKE_HOME_DIRECTORY"]).resolve()
    compiler = cache["CMAKE_C_COMPILER"]
    suffix = "dylib" if platform.system() == "Darwin" else "so"
    probe = build / f"incremental-probe.{suffix}"
    # Ask the configured graph, not the runner checkout: an archived build
    # can lack this target, and a current build can disable benchmark tools.
    targets = subprocess.check_output(["cmake", "--build", str(build), "--target", "help"], text=True)
    if re.search(r"^\s*(?:\.\.\.\s+)?incremental_probe(?:\s|:|$)", targets, re.MULTILINE):
        subprocess.run(["cmake", "--build", str(build), "--target", "incremental_probe"], check=True, capture_output=True)
    else:
        adapter = source / "experiments/incremental/probe.c"
        if not adapter.is_file():
            raise RuntimeError(f"Selected build source has no compatible experiment adapter: {adapter}")
        if (source / "packages/markdown-core/core/diagnostics.h").exists():
            raise RuntimeError("Selected build omits the diagnostic target; configure it with MARKDOWN_CORE_TESTS=ON")
        subprocess.run([
            compiler, "-std=c11", "-O3", "-shared", "-fPIC",
            "-I" + str(source / "packages/markdown-core/include"),
            "-I" + str(source / "packages/markdown-core/core"),
            "-I" + str(source / "packages/markdown-core/elements"),
            "-I" + str(build / "packages/markdown-core/core"),
            str(adapter), str(build / "packages/markdown-core/elements/libmarkdown-core.a"),
            "-o", str(probe),
        ], check=True, capture_output=True)
    return probe, source, compiler


def source_revision(source):
    """An extracted archive has no Git provenance; do not borrow the runner's."""
    try:
        repository = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], cwd=source, text=True, stderr=subprocess.DEVNULL).strip()
        if Path(repository).resolve() != source:
            return None, None
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
        modified = bool(subprocess.check_output(
            ["git", "-c", "core.fsmonitor=false", "status", "--porcelain", "--", "packages/markdown-core"],
            cwd=source, text=True).strip())
        return revision, modified
    except subprocess.CalledProcessError:
        return None, None


class Core:
    def __init__(self, build):
        self.build = build = build.resolve()
        suffix = "dylib" if platform.system() == "Darwin" else "so"
        candidates = sorted((build / "packages/markdown-core/elements").glob(f"libmarkdown-core*.{suffix}"))
        if not candidates:
            raise RuntimeError("Build the benchmark preset first; see README.md")
        self.lib = C.CDLL(str(candidates[0]))
        for name, args, result in (
            ("markdown_core_document_parse", [C.c_char_p, SIZE, POINTER], POINTER),
            ("markdown_core_document_free", [POINTER], None),
            ("markdown_core_document_dump", [POINTER, C.POINTER(POINTER), C.POINTER(SIZE), POINTER], C.c_bool),
            ("markdown_core_dump_free", [POINTER], None),
        ):
            function = getattr(self.lib, name)
            function.argtypes, function.restype = args, result
        probe, self.source, self.compiler = prepare_probe(build)
        self.probe = C.CDLL(str(probe))
        self.probe.mc_probe.argtypes = [C.c_char_p, SIZE, C.POINTER(Stats)]
        self.probe.mc_probe.restype = C.c_int
        self.probe.mc_collisions.argtypes = [SIZE, POINTER, C.POINTER(SIZE), C.POINTER(SIZE)]
        self.probe.mc_collisions.restype = C.c_int

    def parse(self, source):
        return Document(self, source)

    def dump(self, source):
        document = self.parse(source)
        try:
            return document.dump()
        finally:
            document.close()

    def measure(self, source, repeats=7):
        times = []
        for _ in range(repeats + 1):
            start = time.perf_counter_ns()
            document = self.parse(source)
            document.close()
            times.append(time.perf_counter_ns() - start)
        stats = Stats()
        if not self.probe.mc_probe(source, len(source), C.byref(stats)):
            raise AssertionError("instrumented parse failed or leaked native requested bytes")
        return {
            "bytes": len(source), "parse_free_median_ns": int(statistics.median(times[1:])),
            **{key: getattr(stats, key) for key, _ in stats._fields_},
        }


def review(core):
    cases = {}
    for n in (256, 1024, 4096):
        for depth, width in ((n, 1), (1, n), (n, n)):
            source = b"![" * depth + b"a@b.co " * width + b"](u)" * depth
            cases[f"embedded_email_d{depth}_w{width}"] = core.measure(source)
    for n in (1024, 16384, 262144, 1048576):
        cases[f"plain_{n}"] = core.measure(b"a" * n)
        cases[f"sparse_attribute_{n}"] = core.measure(b"[x]{.a} " + b"a" * n)
    for n in (256, 512, 1024, 2048):
        keys, candidates, probes = C.create_string_buffer(n * 18), SIZE(), SIZE()
        if not core.probe.mc_collisions(n, keys, C.byref(candidates), C.byref(probes)):
            raise AssertionError("radix insertion/lookup failed for the baseline collision workload")
        labels = [keys.raw[i * 18:i * 18 + 17] for i in range(n)]
        source = b"".join(b"[" + key + b"]: /u\n" for key in labels) + b"\n[" + labels[0] + b"]\n"
        control = source.replace(b"x", b"y")
        assert probes.value <= n * (9 * 17 + 1)
        cases[f"hash_collision_{n}"] = {
            **core.measure(source), "keys": n, "generation_candidates": candidates.value,
            "verified_lookup_branch_visits": probes.value,
            "equal_length_control": core.measure(control),
        }
    return cases


def records(dump, line_delta=0):
    result = []
    for line in dump.splitlines():
        match = re.match(r"^([ │├└─]*)([A-Za-z].*)$", line)
        if not match:
            raise AssertionError(f"unrecognized canonical dump line: {line}")
        prefix, body = match.groups()
        if line_delta:
            body = re.sub(
                r"scope=(\d+):(\d+)\.\.(\d+):(\d+)",
                lambda m: f"scope={int(m[1]) + line_delta}:{m[2]}..{int(m[3]) + line_delta}:{m[4]}",
                body,
            )
        result.append((len(prefix) // 4, body))
    return result


def witnesses(core):
    # Each pair proves a distinct reason an already published prefix can change.
    pairs = {
        "forward_reference": (b"[x]\n\n", b"[x]: /target\n"),
        "heading_reference": (b"[Title]\n\n", b"# Title\n"),
        "named_footnote": (b"[^x]\n\n", b"[^x]: body\n"),
        "explicit_anchor_reservation": (b"# title\n\n", b"# other {#title}\n"),
        "inline_footnote_id_reservation": (b"^[body]\n\n", b"[^inline-1]: named\n"),
        "percent_comment": (b"%%\n\nold\n\n", b"%%\n"),
        "setext": (b"title\n", b"---\n"),
        "pipe_table": (b"| a | b |\n", b"| - | - |\n"),
        "table_caption_before": (b"Table: caption\n\n", b"| a |\n| - |\n"),
        "table_caption_after": (b"| a |\n| - |\n\n", b"Table: caption\n"),
        "definition_list": (b"term\n\n", b": definition\n"),
        "list_tightness": (b"- a\n\n", b"- b\n"),
        "block_identifier": (b"- item\n\n", b"#identity#\n"),
        "properties_envelope": (b"---\nname: value\n", b"---\n"),
        "fence_blank_line": (b"```\na\n\n", b"b\n```\n"),
        "directive": (b"::: note\na\n\n", b"b\n:::\n"),
    }
    result = {}
    for name, (before, addition) in pairs.items():
        old, new = records(core.dump(before)), records(core.dump(before + addition))
        # Ignore Document's trivially changed extent; retain every node field.
        changed = old[1:] != new[1:len(old)]
        if not changed:
            raise AssertionError(f"witness does not change its old prefix: {name}")
        result[name] = {
            "before": before.decode(), "append": addition.decode(),
            "old_dump": core.dump(before), "new_dump": core.dump(before + addition),
        }
    return result


class ParagraphSession:
    """Admitted experiment: unindented ASCII letter/space prose and LF only.

    Admission is deliberately narrower than Markdown. It establishes that
    blank lines close independent paragraphs, so the unchanged C engine can
    parse islands. This is NOT the proposed production restart algorithm.
    """
    def __init__(self, core):
        self.core = core
        self.frozen = []
        self.tail = b""
        self.tail_document = None
        self.parsed_bytes = 0

    @staticmethod
    def admit(source):
        if not re.fullmatch(rb"(?:[A-Za-z][A-Za-z ]*(?:\n|$)|\n)*", source):
            raise ValueError("outside the independent-prose experiment domain")

    def parse(self, source):
        self.parsed_bytes += len(source)
        return self.core.parse(source)

    def append(self, chunk):
        pending = self.tail + chunk
        self.admit(pending)
        complete = []
        begin = 0
        while True:
            end = pending.find(b"\n\n", begin)
            if end < 0:
                break
            complete.append(pending[begin:end + 2])
            begin = end + 2
        staged = []
        tail_document = None
        try:
            for source in complete:
                staged.append((source, self.parse(source)))
            tail = pending[begin:]
            if tail:
                tail_document = self.parse(tail)
        except Exception:
            for _, document in staged:
                document.close()
            if tail_document:
                tail_document.close()
            raise
        if self.tail_document:
            self.tail_document.close()
        self.frozen.extend(staged)
        self.tail, self.tail_document = tail, tail_document

    def source(self):
        return b"".join(source for source, _ in self.frozen) + self.tail

    def verify(self):
        source = self.source()
        baseline = records(self.core.dump(source))
        flattened, line = [], 0
        islands = self.frozen + ([(self.tail, self.tail_document)] if self.tail_document else [])
        for text, document in islands:
            flattened.extend(records(document.dump(), line)[1:])
            line += text.count(b"\n")
        line_count = source.count(b"\n") + int(bool(source) and not source.endswith(b"\n"))
        last_line = source.split(b"\n")[-2 if source.endswith(b"\n") else -1] if source else b""
        count = sum(depth == 1 for depth, _ in flattened)
        root = f"Document scope=1:1..{line_count}:{len(last_line)} anchor=null attributes={{}} children={count}"
        assert baseline == [(0, root)] + flattened, (baseline, [(0, root)] + flattened)

    def close(self):
        for _, document in self.frozen:
            document.close()
        self.frozen.clear()
        if self.tail_document:
            self.tail_document.close()
            self.tail_document = None


class Fenwick:
    def __init__(self, lengths):
        self.size = len(lengths)
        self.values = [0] * (self.size + 1)
        for i, length in enumerate(lengths):
            self.add(i, length)

    def add(self, index, delta):
        i = index + 1
        while i <= self.size:
            self.values[i] += delta
            i += i & -i

    def prefix(self, end):
        result = 0
        while end:
            result += self.values[end]
            end -= end & -end
        return result

    def locate(self, offset):
        index, total, steps = 0, 0, 0
        bit = 1 << (self.size.bit_length() - 1)
        while bit:
            following = index + bit
            steps += 1
            if following <= self.size and total + self.values[following] <= offset:
                index, total = following, total + self.values[following]
            bit >>= 1
        if index == self.size:
            raise IndexError(offset)
        return index, offset - total, steps


def timings(values):
    ordered = sorted(values)
    return {"total_ms": sum(values) / 1e6, "p50_us": statistics.median(values) / 1e3,
            "p95_us": ordered[math.ceil(len(values) * .95) - 1] / 1e3}


def streaming(core, verify_only=False):
    results = []
    unit = b"alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu\n\n"
    workloads = (("paragraphs", unit * 512), ("long_paragraph", b"alpha beta gamma " * 2048))
    for name, source in workloads:
        for chunk_size in ((1, 7, 64) if verify_only else (16, 64, 256)):
            data = source[:600] if verify_only else source
            session, full_bytes, full_times, local_times = ParagraphSession(core), 0, [], []
            current = b""
            try:
                for offset in range(0, len(data), chunk_size):
                    chunk = data[offset:offset + chunk_size]
                    start = time.perf_counter_ns()
                    current += chunk
                    document = core.parse(current)
                    document.close()
                    full_times.append(time.perf_counter_ns() - start)
                    full_bytes += len(current)
                    start = time.perf_counter_ns()
                    session.append(chunk)
                    local_times.append(time.perf_counter_ns() - start)
                    if verify_only:
                        session.verify()
                session.verify()
                results.append({"case": name, "bytes": len(data), "chunk_bytes": chunk_size,
                                "updates": len(local_times), "full_parsed_bytes": full_bytes,
                                "island_parsed_bytes": session.parsed_bytes,
                                "full": timings(full_times), "island": timings(local_times),
                                "oracle_checked": "every prefix" if verify_only else "final snapshot"})
            finally:
                session.close()
    return results


def editing(core):
    results = []
    for count in (128, 1024, 8192):
        unit = b"alpha beta gamma delta epsilon zeta eta theta\n\n"
        session = ParagraphSession(core)
        session.append(unit * count)
        index = Fenwick([len(source) for source, _ in session.frozen])
        local_times, full_times, full_bytes, lookup_steps = [], [], 0, 0
        initial_parsed = session.parsed_bytes
        source = unit * count
        try:
            for step in range(128):
                target = (step * 103) % count
                start_offset = index.prefix(target) + 1
                replacement = b"XYZ" if step % 2 else b"Q"
                start = time.perf_counter_ns()
                source = source[:start_offset] + replacement + source[start_offset + 1:]
                document = core.parse(source)
                document.close()
                full_times.append(time.perf_counter_ns() - start)
                full_bytes += len(source)
                start = time.perf_counter_ns()
                part, at, steps = index.locate(start_offset)
                text, previous = session.frozen[part]
                updated = text[:at] + replacement + text[at + 1:]
                session.admit(updated)
                document = session.parse(updated)
                session.frozen[part] = updated, document
                index.add(part, len(updated) - len(text))
                previous.close()
                local_times.append(time.perf_counter_ns() - start)
                lookup_steps += steps
                if count == 128:
                    session.verify()
            assert source == session.source()
            session.verify()
            results.append({"paragraphs": count, "initial_bytes": len(unit) * count, "edits": 128,
                            "full_parsed_bytes": full_bytes,
                            "island_parsed_bytes": session.parsed_bytes - initial_parsed,
                            "lookup_steps": lookup_steps,
                            "full": timings(full_times), "island": timings(local_times),
                            "oracle_checked": "every edit" if count == 128 else "final snapshot"})
        finally:
            session.close()
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/benchmark")
    parser.add_argument("--mode", choices=("all", "review", "witnesses", "incremental"), default="all")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    core = Core(args.build.resolve())
    revision, modified = source_revision(core.source)
    result = {
        "baseline": revision, "source_modified": modified,
        "build_directory": str(core.build), "source_directory": str(core.source),
        "library_sha256": hashlib.sha256(Path(core.lib._name).read_bytes()).hexdigest(),
        "platform": platform.platform(), "machine": platform.machine(), "python": platform.python_version(),
        "compiler": subprocess.check_output([core.compiler, "--version"], text=True).splitlines()[0],
        "timing": "Uninstrumented public C parse + free and Python orchestration; no dumps or rendering. One warmup and seven samples for review; incremental sequence timings are one diagnostic run.",
        "allocation": "Separate instrumented internal parse; includes one recorder registry attachment; requested capacity only; realloc transient old+new and allocator headers excluded.",
    }
    if args.mode in ("all", "review"):
        result["review"] = review(core)
    if args.mode in ("all", "witnesses"):
        result["witnesses"] = witnesses(core)
    if args.mode in ("all", "incremental"):
        result["prefix_checks"] = streaming(core, verify_only=True)
        result["streaming"] = streaming(core)
        result["editing"] = editing(core)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=4) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
