import assert from "node:assert/strict";
import test from "node:test";

import { mergeParts } from "../edits.mjs";

/* A set of `count` workloads measured in `shards` parts: workload k is the
 * (k / shards)th result of part k mod shards. */
const parts = (count, shards) =>
    Array.from({ length: shards }, (_, index) => ({
        schemaVersion: 2,
        subjects: ["reparse", "session"],
        toolchain: { compiler: "cc" },
        binaries: { "markdown-core": { sha256: "a" }, "markdown-core edits": { sha256: "b" } },
        profile: { compiler: "cc", flags: "-O2" },
        workloads: { version: "v1", set: "corpus", digest: "d", count },
        shard: { index, count: shards },
        results: Array.from({ length: count }, (_, position) => ({ name: `w${position}` })).filter(
            (_, position) => position % shards === index
        )
    }));

test("merged parts hold every workload once, in the set's order", () => {
    const merged = mergeParts(parts(10, 3).reverse());
    assert.deepEqual(
        merged.results.map((row) => row.name),
        Array.from({ length: 10 }, (_, position) => `w${position}`)
    );
    assert.equal(merged.shard, undefined);
    assert.equal(merged.workloads.count, 10);
});

test("a missing, repeated or foreign part is refused", () => {
    assert.throws(() => mergeParts(parts(10, 3).slice(1)), /2 of 3 parts/);
    const repeated = parts(10, 3);
    repeated[2] = repeated[1];
    assert.throws(() => mergeParts(repeated), /not one set of shards/);
    const foreign = parts(10, 3);
    foreign[1].binaries = { ...foreign[1].binaries, "markdown-core": { sha256: "c" } };
    assert.throws(() => mergeParts(foreign), /differ in binaries/);
    const short = parts(10, 3);
    short[0].results.pop();
    assert.throws(() => mergeParts(short), /no part holds workload/);
    const long = parts(10, 3);
    long[0].results.push({ name: "extra" });
    assert.throws(() => mergeParts(long), /more results/);
});
