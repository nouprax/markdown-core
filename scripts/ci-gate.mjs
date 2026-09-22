// A required context always runs, including when validation is reused. Only
// a successful preflight with an explicit false permits skipped dependencies.
export function requireGate(needs) {
    const { changes, ...dependencies } = needs;
    if (changes?.result !== "success") throw new Error("CI input preflight did not succeed");
    const required = changes.outputs?.required;
    if (required !== "true" && required !== "false") throw new Error("Missing or invalid CI input decision");
    if (!Object.keys(dependencies).length) throw new Error("Required gate has no validation dependencies");
    for (const [name, job] of Object.entries(dependencies)) {
        if (job.result !== "success" && !(required === "false" && job.result === "skipped")) {
            throw new Error(`${name} concluded: ${job.result ?? "missing"}`);
        }
    }
    return required === "false"
        ? "Validation skipped or reused after successful CI input preflight."
        : "Every required validation dependency succeeded.";
}
