import assert from "node:assert/strict";
import { Buffer } from "node:buffer";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { parse } from "yaml";
import { attributeSection, publish, readArchive, stageSection } from "../benchmark-comment.mjs";

const head = "a".repeat(40);
const base = "b".repeat(40);
function stageReport(ir) {
    return {
        schemaVersion: 4,
        revision: base,
        corpus: { digest: "c".repeat(64), cases: 1 },
        pairingDigest: "d".repeat(64),
        cases: [
            {
                case: "inline-links-flat",
                scale: 1,
                bytes: 32,
                sha256: "e".repeat(64),
                engines: {
                    "markdown-core": {
                        stages: { source_to_buffer: { cost: { Ir: ir } }, buffer_to_ast: { cost: { Ir: 50 } } },
                        parsePathIr: ir + 60,
                        outsideStagesIr: 10
                    }
                }
            }
        ]
    };
}
const attributes = () => ({
    schemaVersion: 1,
    baselines: {
        "markdown-core": { lists: 10, values: 30, ir: 200, dataReads: 100, dataWrites: 40 },
        lexbor: { lists: 10, values: 30, ir: 100, dataReads: 50, dataWrites: 20 }
    }
});

test("PR tables report numeric results, source regressions, and complete parse accounting", () => {
    const body = stageSection(stageReport(103), stageReport(100));
    assert.match(body, /0\/1 passed/);
    assert.match(body, /\| Source → buffer \| 100 \| 103 \| 1.0300× \|/);
    assert.match(body, /\| Complete parse path \| 160 \| 163 \|/);
    assert.match(body, /inline-links-flat/);
    assert.match(body, /Required when CI inputs require execution/);
    assert.match(attributeSection(attributes()), /2.0000×/);
});

test("report projections reject corrupt counts, mismatched workloads and injected text", () => {
    for (const mutate of [
        (r) => {
            r.schemaVersion = 3;
        },
        (r) => {
            r.cases[0].case = "@everyone <script>";
        },
        (r) => {
            r.corpus.digest = "[click](https://example.com)";
        },
        (r) => {
            r.corpus.cases = 2;
        },
        (r) => {
            r.cases[0].scale = -1;
        },
        (r) => {
            r.cases[0].bytes = 33;
        },
        (r) => {
            r.cases[0].engines["markdown-core"].parsePathIr = NaN;
        },
        (r) => {
            r.cases[0].engines["markdown-core"].parsePathIr = 0;
        },
        (r) => {
            r.cases[0].engines["markdown-core"].stages.source_to_buffer.cost.Ir = "100";
        },
        (r) => {
            r.cases[0].sha256 = "f".repeat(64);
        }
    ]) {
        const report = stageReport(100);
        mutate(report);
        assert.throws(() => stageSection(report, stageReport(100)));
    }
    for (const value of ["200", -1, null, Number.MAX_SAFE_INTEGER + 1]) {
        const report = attributes();
        report.baselines.lexbor.ir = value;
        assert.throws(() => attributeSection(report));
    }
    const different = attributes();
    different.baselines.lexbor.values++;
    assert.throws(() => attributeSection(different));
    const untrusted = stageReport(100);
    untrusted.toolchain = { compiler: "@everyone" };
    assert.doesNotMatch(stageSection(untrusted, stageReport(100)), /@everyone/);
});

test("archives read only fixed JSON members without extracting or executing files", (t) => {
    const cwd = fs.mkdtempSync(path.join(os.tmpdir(), "benchmark-archive-test-"));
    t.after(() => fs.rmSync(cwd, { recursive: true, force: true }));
    fs.writeFileSync(path.join(cwd, "attributes.json"), JSON.stringify(attributes()));
    fs.writeFileSync(path.join(cwd, "untrusted.sh"), "exit 1\n");
    execFileSync("zip", ["-q", "report.zip", "attributes.json", "untrusted.sh"], { cwd });
    const bytes = fs.readFileSync(path.join(cwd, "report.zip"));
    assert.deepEqual(readArchive(bytes, ["attributes.json"]), [attributes()]);
    assert.throws(() => readArchive(bytes, ["stages.json"]));
    assert.throws(() => readArchive(Buffer.alloc(8 * 1024 * 1024 + 1), ["attributes.json"]));
    fs.writeFileSync(path.join(cwd, "attributes.json"), "x".repeat(16 * 1024 * 1024 + 1));
    execFileSync("zip", ["-q", "report.zip", "attributes.json"], { cwd });
    assert.throws(() => readArchive(fs.readFileSync(path.join(cwd, "report.zip")), ["attributes.json"]));
});

function fixture() {
    const run = {
        id: 42,
        run_attempt: 1,
        head_sha: head,
        head_branch: "contribution",
        head_repository: { id: 100 },
        path: ".github/workflows/ci.yml",
        event: "pull_request",
        status: "completed",
        conclusion: "success",
        pull_requests: []
    };
    const pr = {
        number: 9,
        state: "open",
        base: { repo: { full_name: "nouprax/markdown-core" } },
        head: { sha: head, ref: "contribution", repo: { id: 100 } }
    };
    const state = {
        run,
        pr,
        current: JSON.parse(JSON.stringify(pr)),
        latest: JSON.parse(JSON.stringify(run)),
        comments: [],
        writes: [],
        warnings: [],
        associated: [pr],
        artifacts: ["stages", "attributes"].map((kind, id) => ({
            id,
            name: `benchmark-report-${kind}-1`,
            expired: false,
            size_in_bytes: 100
        })),
        jobs: [
            "Benchmark / Measure - parse stages against cmark",
            "Benchmark / Measure - the attribute grammar against lexbor"
        ].map((name) => ({ name, status: "completed", conclusion: "success", run_attempt: 1 }))
    };
    const api = {
        repos: { listPullRequestsAssociatedWithCommit: () => state.associated },
        pulls: { get: async () => ({ data: state.current }) },
        actions: {
            listWorkflowRunArtifacts: () => state.artifacts,
            listJobsForWorkflowRun: () => state.jobs,
            getWorkflowRun: async () => ({ data: state.latest }),
            downloadArtifact: async ({ artifact_id }) => ({ data: Buffer.from(String(artifact_id)) })
        },
        issues: {
            listComments: () => state.comments,
            createComment: async (args) => state.writes.push({ method: "create", ...args }),
            updateComment: async (args) => state.writes.push({ method: "update", ...args })
        }
    };
    state.publish = () =>
        publish({
            github: { rest: api, paginate: async (method, args) => method(args) },
            context: {
                eventName: "workflow_run",
                repo: { owner: "nouprax", repo: "markdown-core" },
                payload: { workflow_run: run }
            },
            core: { warning: (message) => state.warnings.push(message) },
            read: (bytes) => (bytes.toString() === "0" ? [stageReport(103), stageReport(100)] : [attributes()])
        });
    return state;
}

test("fork runs with empty PR metadata find the current PR through commit association", async () => {
    const state = fixture();
    state.run.conclusion = "failure"; // A failed source budget still has results.
    await state.publish();
    assert.equal(state.writes.length, 1);
    assert.equal(state.writes[0].issue_number, 9);
    assert.match(state.writes[0].body, /0\/1 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
    assert.match(state.writes[0].body, /CI status: \*\*failure\*\*/);
    assert.match(state.writes[0].body, /runs\/42\/attempts\/1/);
});

test("one bot-owned comment is updated, without editing a user's marker imitation", async () => {
    const state = fixture();
    await state.publish();
    const body = state.writes[0].body;
    state.writes.length = 0;
    state.comments = [
        { id: 8, body, user: { login: "contributor", type: "User" } },
        { id: 10, body, user: { login: "github-actions[bot]", type: "Bot" } }
    ];
    await state.publish();
    assert.equal(state.writes.length, 1);
    assert.equal(state.writes[0].method, "update");
    assert.equal(state.writes[0].comment_id, 10);
});

test("obsolete, foreign, closed and superseded results never replace the current comment", async () => {
    for (const mutate of [
        (s) => {
            s.current.head.sha = base;
        },
        (s) => {
            s.current.state = "closed";
        },
        (s) => {
            s.pr.head.repo.id = 200;
        },
        (s) => {
            s.pr.base.repo.full_name = "someone/else";
        },
        (s) => {
            s.pr.head.ref = "another-branch";
        },
        (s) => {
            s.run.event = "push";
        },
        (s) => {
            s.run.path = ".github/workflows/other.yml";
        },
        (s) => {
            s.latest.run_attempt = 2;
        },
        (s) => {
            s.latest.status = "in_progress";
        },
        (s) => {
            s.comments = [
                {
                    id: 1,
                    user: { login: "github-actions[bot]", type: "Bot" },
                    body: "<!-- markdown-core-benchmark -->\n<!-- run:43:1 -->"
                }
            ];
        },
        (s) => {
            s.comments = [
                {
                    id: 1,
                    user: { login: "github-actions[bot]", type: "Bot" },
                    body: "<!-- markdown-core-benchmark -->\n<!-- run:42:2 -->"
                }
            ];
        }
    ]) {
        const state = fixture();
        mutate(state);
        await state.publish();
        assert.equal(state.writes.length, 0);
    }
});

test("missing, corrupt or expired artifacts produce an explicit partial result", async () => {
    for (const mutate of [
        (s) => {
            s.artifacts.shift();
        },
        (s) => {
            s.artifacts[0].expired = true;
        },
        (s) => {
            s.artifacts[0].size_in_bytes = 8 * 1024 * 1024 + 1;
        },
        (s) => {
            s.artifacts.push(s.artifacts[0]);
        },
        (s) => {
            s.artifacts[0].name = "benchmark-report-stages-0";
        },
        (s) => {
            s.artifacts[0].id = 2;
        }
    ]) {
        const state = fixture();
        mutate(state);
        await state.publish();
        assert.equal(state.writes.length, 1);
        assert.match(state.writes[0].body, /Result unavailable/);
        assert.match(state.writes[0].body, /2.0000×/);
        assert.equal(state.warnings.length, 1);
    }
});

test("a docs-only skip preserves the last measured comment and posts no empty report", async () => {
    const state = fixture();
    state.artifacts = [];
    state.jobs.forEach((job) => {
        job.conclusion = "skipped";
    });
    await state.publish();
    assert.equal(state.writes.length, 0);
});

test("failed-job retries retain successful measurements but cannot reuse a failed retry's old artifact", async () => {
    const state = fixture();
    state.run.run_attempt = state.latest.run_attempt = 2;
    state.jobs[0].run_attempt = 2;
    state.artifacts[0].name = "benchmark-report-stages-2";
    // Attribute job and artifact still belong to attempt 1.
    await state.publish();
    assert.match(state.writes[0].body, /0\/1 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
    assert.match(state.writes[0].body, /runs\/42\/attempts\/2/);
    assert.equal(state.warnings.length, 0);
    state.writes.length = 0;
    state.artifacts[0].name = "benchmark-report-stages-1";
    await state.publish();
    assert.match(state.writes[0].body, /Result unavailable/);
    assert.doesNotMatch(state.writes[0].body, /0\/1 passed/);
    assert.match(state.writes[0].body, /2.0000×/);
});

test("a skipped reusable Benchmark call preserves the comment", async () => {
    const state = fixture();
    state.jobs = [{ name: "Benchmark", conclusion: "skipped" }];
    await state.publish();
    assert.equal(state.writes.length, 0);
});

test("producer and publisher keep PR execution separate from write permissions", () => {
    const workflow = (file) =>
        parse(fs.readFileSync(new URL(`../../.github/workflows/${file}.yml`, import.meta.url), "utf8"));
    const producer = workflow("benchmark");
    assert.ok(Object.values(producer.permissions).every((value) => value === "read"));
    for (const job of Object.values(producer.jobs)) {
        assert.ok(Object.values(job.permissions ?? {}).every((value) => value === "read"));
        for (const step of job.steps ?? []) {
            assert.doesNotMatch(step.run ?? "", /GITHUB_STEP_SUMMARY/);
            if (step.uses?.startsWith("actions/checkout@")) assert.equal(step.with["persist-credentials"], false);
        }
    }
    const publisher = workflow("benchmark-comment");
    assert.deepEqual(publisher.on, { workflow_run: { workflows: ["CI"], types: ["completed"] } });
    assert.equal(publisher.permissions["pull-requests"], "write");
    assert.equal(publisher.concurrency["cancel-in-progress"], false);
    const steps = publisher.jobs.comment.steps;
    const checkout = steps.find((step) => step.uses?.startsWith("actions/checkout@"));
    assert.equal(checkout.with.ref, "${{ github.sha }}");
    assert.equal(checkout.with["persist-credentials"], false);
    assert.equal(steps.length, 2);
    assert.ok(steps.every((step) => !step.run));
    assert.match(steps[1].with.script, /scripts\/benchmark-comment\.mjs/);
    for (const [kind, members] of [
        ["stages", ["stages.json", "baseline/stages.json"]],
        ["attributes", ["attributes.json"]]
    ]) {
        const upload = producer.jobs[kind].steps.find(
            (step) => step.with?.name === `benchmark-report-${kind}-` + "${{ github.run_attempt }}"
        );
        assert.ok(upload);
        assert.equal(upload.if, "${{ always() }}");
        for (const member of members) assert.ok(upload.with.path.includes(member));
    }
});
