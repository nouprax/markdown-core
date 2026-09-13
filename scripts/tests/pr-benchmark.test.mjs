import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { parse } from "yaml";
import {
    ARTIFACT,
    MAX_RESULT_BYTES,
    ORDER,
    pairedEstimate,
    renderComparison,
    summarize,
    validateComparison
} from "../pr-benchmark-result.mjs";
import { publishComparison, resolveComparison } from "../pr-benchmark-comment.mjs";

const baseSha = "a".repeat(40),
    headSha = "b".repeat(40),
    hash = "c".repeat(64);
const expected = { pullNumber: 275, baseSha, headSha, run: { id: 47, attempt: 1 } };

function fixture() {
    const binary = {
        libraryBytes: 10000,
        librarySha256: hash,
        runnerSha256: hash,
        cmakeCacheSha256: hash,
        compileCommandsSha256: hash
    };
    return {
        schema: 2,
        baseSha,
        headSha,
        run: { id: 47, attempt: 1, job: "compare" },
        environment: {
            os: "linux",
            arch: "x64",
            release: "test",
            cpu: "test",
            logicalCpus: 2,
            compiler: "clang",
            cmake: "cmake",
            runnerImage: "ubuntu",
            runnerImageVersion: "test",
            cpuAffinity: 0,
            loadBefore: [1, 1, 1],
            loadAfter: [1, 1, 1]
        },
        workload: { name: "representative_large", bytes: 158000, sha256: hash, canonicalSha256: hash },
        harnessSha256: hash,
        settings: { blocks: 2, warmup: 5, repeats: 3, order: ORDER },
        binaries: { base: { ...binary }, head: { ...binary } },
        blocks: Array.from({ length: 2 }, () =>
            ORDER.map((lane) => ({
                lane,
                rssKiB: 1000,
                parseNs: Array(3).fill(lane === "head" ? 110 : 100),
                freeNs: [20, 20, 20]
            }))
        )
    };
}

test("paired results retain raw parse/free samples and derive total work", () => {
    const value = fixture();
    assert.equal(validateComparison(value, expected), value);
    const { phases } = summarize(value);
    assert.equal(phases.parse.base, 100);
    assert.equal(phases.parse.head, 110);
    assert.ok(Math.abs(phases.parse.ratio - 1.1) < 1e-12);
    assert.ok(Math.abs(phases.total.ratio - 130 / 120) < 1e-12);
    assert.equal(phases.free.ratio, 1);
});

test("ABBA pairing cancels a multiplicative linear time trend", () => {
    const value = fixture();
    for (const block of value.blocks) {
        block.forEach((run, i) => {
            run.parseNs = [100, 100, 100].map((n) => n * 2 ** i);
        });
    }
    const result = summarize(value).phases.parse;
    assert.equal(result.ratio, 1);
    assert.equal(result.low, 1);
    assert.equal(result.high, 1);
});

test("whole-block bootstrap is deterministic and exposes between-block variation", () => {
    const logs = [Math.log(0.8), Math.log(1.2), Math.log(1.05), Math.log(0.95)];
    const a = pairedEstimate(logs),
        b = pairedEstimate(logs);
    assert.deepEqual(a, b);
    assert.ok(a.low < 1 && a.high > 1);
});

test("malformed, incomplete, mixed-provenance and legacy measurements are rejected", () => {
    const mutations = [
        (v) => {
            v.schema = 1;
        },
        (v) => {
            v.baseSha = headSha;
        },
        (v) => {
            v.headSha = baseSha;
        },
        (v) => {
            v.run.id++;
        },
        (v) => {
            v.run.attempt++;
        },
        (v) => {
            v.origin = "main-build";
        },
        (v) => {
            v.binaries.head.runnerSha256 = "d".repeat(64);
        },
        (v) => {
            v.blocks.pop();
        },
        (v) => {
            v.blocks[0].pop();
        },
        (v) => {
            v.blocks[0][0].lane = "head";
        },
        (v) => {
            v.blocks[0][0].parseNs.pop();
        },
        (v) => {
            v.blocks[0][0].parseNs[0] = -1;
        },
        (v) => {
            v.blocks[0][0].freeNs[0] = Infinity;
        },
        (v) => {
            v.blocks[0][0].freeNs[0] = 1.5;
        },
        (v) => {
            v.blocks[0][0].rssKiB = 0;
        },
        (v) => {
            v.workload.sha256 = "not a digest";
        },
        (v) => {
            v.environment.cpu = "x".repeat(3000);
        }
    ];
    for (const mutate of mutations) {
        const value = fixture();
        mutate(value);
        assert.throws(() => validateComparison(value, expected));
    }
});

test("artifact metadata never becomes executable code or Markdown in the report", () => {
    const value = fixture();
    value.environment.cpu = "@someone [click](javascript:alert(1))";
    const report = renderComparison(value);
    assert.ok(!report.includes(value.environment.cpu));
    assert.match(report, /parse \+ free/);
    assert.match(report, /95% interval/);
    assert.match(report, /Both lanes are PR-controlled/);
});

function api(t) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "benchmark-comment-test-"));
    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
    const file = path.join(dir, "comparison.json");
    const value = fixture();
    fs.writeFileSync(file, JSON.stringify(value));
    const pull = {
        number: 275,
        state: "open",
        head: { sha: headSha },
        base: { sha: baseSha, repo: { full_name: "owner/repo" } }
    };
    const artifacts = [{ name: `${ARTIFACT}-1`, size_in_bytes: 1000, expired: false }];
    const writes = [];
    const github = {
        rest: {
            actions: { listWorkflowRunArtifacts: "artifacts" },
            repos: { listPullRequestsAssociatedWithCommit: async () => ({ data: [pull] }) },
            pulls: { get: async () => ({ data: pull }) },
            issues: {
                listComments: "comments",
                createComment: async (v) => writes.push(v),
                updateComment: async (v) => writes.push(v)
            }
        },
        paginate: async (method) => (method === "artifacts" ? artifacts : [])
    };
    const context = {
        repo: { owner: "owner", repo: "repo" },
        payload: {
            workflow_run: {
                id: 47,
                run_attempt: 1,
                path: ".github/workflows/pr-benchmark.yml",
                event: "pull_request",
                status: "completed",
                conclusion: "success",
                head_sha: headSha,
                pull_requests: [{ number: 275 }]
            }
        }
    };
    return { github, context, core: { notice() {}, warning() {} }, expected, file, pull, artifacts, writes };
}

test("only the current successful run's bounded pair is eligible", async (t) => {
    const f = api(t);
    assert.deepEqual(await resolveComparison(f), expected);
    f.context.payload.workflow_run.conclusion = "failure";
    assert.equal(await resolveComparison(f), null);
    f.context.payload.workflow_run.conclusion = "success";
    f.artifacts[0].size_in_bytes = MAX_RESULT_BYTES + 1;
    assert.equal(await resolveComparison(f), null);
    f.artifacts[0].size_in_bytes = 1000;
    f.artifacts.push({ ...f.artifacts[0] });
    assert.equal(await resolveComparison(f), null);
});

test("valid results publish, while a base or head advance prevents publication", async (t) => {
    const f = api(t);
    assert.equal(await publishComparison(f), true);
    assert.equal(f.writes.length, 1);
    for (const side of ["base", "head"]) {
        const old = f.pull[side].sha;
        f.pull[side].sha = "e".repeat(40);
        assert.equal(await publishComparison(f), false);
        f.pull[side].sha = old;
    }
    assert.equal(f.writes.length, 1);
});

test("oversized, symbolic and invalid artifacts cannot publish", async (t) => {
    const f = api(t);
    fs.writeFileSync(f.file, "x".repeat(MAX_RESULT_BYTES + 1));
    assert.equal(await publishComparison(f), false);
    fs.writeFileSync(f.file, "{}");
    assert.equal(await publishComparison(f), false);
    fs.renameSync(f.file, `${f.file}.other`);
    fs.symlinkSync(`${f.file}.other`, f.file);
    assert.equal(await publishComparison(f), false);
    assert.equal(f.writes.length, 0);
});

test("workflow isolates same-job measurement from privileged reporting", () => {
    const producer = parse(fs.readFileSync(".github/workflows/pr-benchmark.yml", "utf8"));
    const reporter = parse(fs.readFileSync(".github/workflows/pr-benchmark-comment.yml", "utf8"));
    assert.deepEqual(Object.keys(producer.on), ["pull_request"]);
    assert.deepEqual(producer.permissions, { actions: "read", contents: "read" });
    assert.deepEqual(Object.keys(producer.jobs), ["changes", "compare"]);
    const checkouts = producer.jobs.compare.steps.filter((s) => s.uses?.startsWith("actions/checkout@"));
    assert.deepEqual(
        checkouts.map((s) => s.with.ref),
        ["${{ github.event.pull_request.base.sha }}", "${{ github.event.pull_request.head.sha }}"]
    );
    assert.ok(checkouts.every((s) => s.with["persist-credentials"] === false));
    const runSteps = producer.jobs.compare.steps.filter((s) => s.run);
    assert.equal(runSteps.length, 1);
    assert.match(runSteps[0].run, /--base-source sources\/base/);
    assert.match(runSteps[0].run, /--head-source sources\/head/);
    assert.ok(!JSON.stringify(producer).includes("download-artifact"));
    assert.deepEqual(reporter.permissions, { actions: "read", contents: "read", "pull-requests": "write" });
    const trusted = reporter.jobs.comment.steps.filter((s) => s.uses?.startsWith("actions/checkout@"));
    assert.equal(trusted.length, 1);
    assert.equal(trusted[0].with.ref, "${{ github.workflow_sha }}");
    assert.equal(trusted[0].with["persist-credentials"], false);
    assert.ok(!reporter.jobs.comment.steps.some((s) => s.run));
    assert.ok(!JSON.stringify(reporter).includes("pr-benchmark-baseline"));
    assert.ok(!JSON.stringify(reporter).includes("cmake"));
    const download = reporter.jobs.comment.steps.find((s) => s.uses?.startsWith("actions/download-artifact@"));
    assert.equal(download.with["run-id"], "${{ github.event.workflow_run.id }}");
    assert.equal(download.with.name, `${ARTIFACT}-\${{ github.event.workflow_run.run_attempt }}`);
});
