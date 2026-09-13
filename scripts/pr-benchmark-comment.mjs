import fs from "node:fs";
import { ARTIFACT, MARKER, MAX_RESULT_BYTES, renderComparison, validateComparison } from "./pr-benchmark-result.mjs";

export async function resolveComparison({ github, context, core }) {
    const run = context.payload.workflow_run;
    if (
        run.event !== "pull_request" ||
        run.status !== "completed" ||
        run.conclusion !== "success" ||
        run.path !== ".github/workflows/pr-benchmark.yml"
    )
        return null;
    const artifacts = await github.paginate(github.rest.actions.listWorkflowRunArtifacts, {
        ...context.repo,
        run_id: run.id,
        per_page: 100
    });
    const candidates = artifacts.filter((artifact) => artifact.name === `${ARTIFACT}-${run.run_attempt}`);
    if (
        candidates.length !== 1 ||
        candidates[0].expired ||
        candidates[0].size_in_bytes <= 0 ||
        candidates[0].size_in_bytes > MAX_RESULT_BYTES
    ) {
        core.notice("No bounded paired measurement is available for this run.");
        return null;
    }
    let pulls = run.pull_requests ?? [];
    if (pulls.length !== 1) {
        const associated = await github.rest.repos.listPullRequestsAssociatedWithCommit({
            ...context.repo,
            commit_sha: run.head_sha
        });
        pulls = associated.data.filter(
            (pull) =>
                pull.base.repo.full_name === `${context.repo.owner}/${context.repo.repo}` &&
                pull.head.sha === run.head_sha
        );
    }
    if (pulls.length !== 1) return null;
    const { data: pull } = await github.rest.pulls.get({ ...context.repo, pull_number: pulls[0].number });
    if (
        pull.state !== "open" ||
        pull.base.repo.full_name !== `${context.repo.owner}/${context.repo.repo}` ||
        pull.head.sha !== run.head_sha
    ) {
        core.notice("The benchmark run no longer matches an open PR head.");
        return null;
    }
    return {
        pullNumber: pull.number,
        baseSha: pull.base.sha,
        headSha: pull.head.sha,
        run: { id: run.id, attempt: run.run_attempt }
    };
}

export async function publishComparison({ github, context, core, expected, file }) {
    let value;
    try {
        const stat = fs.lstatSync(file);
        if (!stat.isFile() || stat.isSymbolicLink() || stat.size > MAX_RESULT_BYTES)
            throw new Error("invalid artifact file");
        value = validateComparison(JSON.parse(fs.readFileSync(file, "utf8")), expected);
    } catch (error) {
        core.warning(`No valid paired comparison: ${error.message}`);
        return false;
    }
    // Recheck both SHAs immediately before publishing. The base can advance
    // even when the head is unchanged; never relabel an old pair as current.
    const { data: pull } = await github.rest.pulls.get({ ...context.repo, pull_number: expected.pullNumber });
    if (
        pull.state !== "open" ||
        pull.head.sha !== expected.headSha ||
        pull.base.sha !== expected.baseSha ||
        pull.base.repo.full_name !== `${context.repo.owner}/${context.repo.repo}`
    ) {
        core.notice("The measured base/head pair is stale; no comment was changed.");
        return false;
    }
    const runUrl = `https://github.com/${context.repo.owner}/${context.repo.repo}/actions/runs/${expected.run.id}`;
    const body = renderComparison(value) + `\n[Benchmark run and raw artifacts](${runUrl})\n`;
    const comments = await github.paginate(github.rest.issues.listComments, {
        ...context.repo,
        issue_number: expected.pullNumber,
        per_page: 100
    });
    const previous = comments.find((comment) => comment.user?.type === "Bot" && comment.body?.includes(MARKER));
    if (previous) {
        await github.rest.issues.updateComment({ ...context.repo, comment_id: previous.id, body });
    } else {
        await github.rest.issues.createComment({ ...context.repo, issue_number: expected.pullNumber, body });
    }
    return true;
}
