"""Replay the old attribute grammar against the candidate, in several query orders."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import random
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = ["markdown_core_attributes_free", "markdown_core_attribute_parser_free",
             "markdown_core_attributes_end", "markdown_core_attributes_tail", "markdown_core_attributes_parse"]
WRAPPER = """
int oracle_ends(const unsigned char *data, int length, const int *queries, int count, int *ends) {
    markdown_core_attribute_parser p = {.mem = markdown_core_get_default_mem_allocator(), .data = data, .length = length};
    for (int i = 0; i < count; i++) ends[i] = markdown_core_attributes_end(&p, queries[i]);
    markdown_core_attribute_parser_free(&p);
    return !p.oom;
}
"""


def inputs(rng, count):
    yield b""
    for n in (1, 2, 8, 256, 4096):
        yield b"{k=" + b"{x=" * n + b"v " + b".a " * n + b"}"
    for n in (1, 2, 8, 256):
        yield b'{k="' + b"{x='" * n + b"v " + b".a " * n + b"}"
    tokens = ['{', '}', ' ', '\t', '\r', '\n', '\r\n', '\n \n', '\\', '\\}', '\\{', '\\"', "\\'",
              'a', '-', '.', '#', '=', '"', "'", 'k=', 'k="', "k='", '.foo', '#x', 'foo-bar',
              '文', 'é', '😀', 'é́', '&amp;', '[x]']
    for _ in range(count):
        yield ''.join(rng.choices(tokens, k=rng.randrange(1, 180))).encode()


def main():
    arg = argparse.ArgumentParser(description=__doc__)
    arg.add_argument("--baseline-revision", default="84efce3220e6e88fee1e6d5954b88eca82960924")
    arg.add_argument("--build", type=Path, default=ROOT / "build/benchmark")
    arg.add_argument("--seed", type=int, default=273)
    arg.add_argument("--random-inputs", type=int, default=35000)
    arg.add_argument("--output", type=Path, required=True)
    args = arg.parse_args()
    source = subprocess.check_output(["git", "show", f"{args.baseline_revision}:packages/markdown-core/elements/attributes.c"], cwd=ROOT, text=True)
    header = subprocess.check_output(["git", "show", f"{args.baseline_revision}:packages/markdown-core/core/attributes.h"], cwd=ROOT, text=True)
    old = source[:source.index('#include "inline_internal.h"')]
    old = old.replace('#include "attributes.h"\n', '').replace('#include "../core/attributes.h"', '#include "oracle.h"')
    old += WRAPPER
    for name in FUNCTIONS:
        old = old.replace(name, "baseline_" + name)
    new = '#include "attributes.h"\n' + WRAPPER
    with tempfile.TemporaryDirectory(prefix="markdown-core-attribute-oracle-") as directory:
        temp = Path(directory)
        (temp / "oracle.h").write_text(header)
        libs = []
        for lane, code in enumerate((old, new)):
            path = temp / f"lane{lane}.c"
            path.write_text(code)
            library = temp / f"lane{lane}.dylib"
            subprocess.run(["cc", "-std=c11", "-O3", "-shared", "-fPIC",
                            "-I" + str(ROOT / "packages/markdown-core/core"),
                            "-I" + str(ROOT / "packages/markdown-core/include"),
                            "-I" + str(args.build / "packages/markdown-core/core"),
                            "-include", str(ROOT / "packages/markdown-core/core/markdown-core.h"),
                            str(path), str(args.build / "packages/markdown-core/elements/libmarkdown-core.a"),
                            "-o", str(library)], check=True, capture_output=True)
            lib = C.CDLL(str(library))
            lib.oracle_ends.argtypes = [C.c_char_p, C.c_int, C.POINTER(C.c_int), C.c_int, C.POINTER(C.c_int)]
            lib.oracle_ends.restype = C.c_int
            libs.append(lib)
        rng = random.Random(args.seed)
        corpus = list(inputs(rng, args.random_inputs))
        queries = 0
        digest = hashlib.sha256()
        for source in corpus:
            digest.update(len(source).to_bytes(8, "little") + source)
            starts = [i for i, byte in enumerate(source) if byte == ord('{')]
            for order in (starts, list(reversed(starts)), rng.sample(starts, len(starts))):
                order = order * 2 + [-1, len(source), len(source) + 1]
                array = (C.c_int * len(order))(*order)
                outputs = [(C.c_int * len(order))() for _ in libs]
                for lib, output in zip(libs, outputs):
                    assert lib.oracle_ends(source, len(source), array, len(order), output), "OOM in differential test"
                if list(outputs[0]) != list(outputs[1]):
                    raise AssertionError((source, order, list(outputs[0]), list(outputs[1])))
                queries += len(order)
    result = {"baseline_revision": args.baseline_revision, "seed": args.seed,
              "inputs": len(corpus), "queries": queries, "orders": ["forward", "reverse", "shuffled", "repeated"],
              "corpus_sha256": digest.hexdigest(), "all_equivalent": True,
              "candidate_library_sha256": hashlib.sha256((args.build / "packages/markdown-core/elements/libmarkdown-core.a").read_bytes()).hexdigest()}
    args.output.write_text(json.dumps(result, indent=4) + "\n")
    print(args.output)


if __name__ == "__main__":
    main()
