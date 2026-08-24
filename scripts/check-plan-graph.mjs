// The reconstruction plan's dependency graph must be acyclic, and every step it
// names must exist. A previous revision carried a 9b<->11 cycle through several
// revisions because the arrows were read one row at a time rather than checked.
import { readFile } from "node:fs/promises";
import process from "node:process";

const doc = await readFile("docs/RECONSTRUCTION.md", "utf8");
const block = doc.match(/Edge list \(`step: \[what must already be true\]`\):\n\n```\n([\s\S]*?)```/);
if (!block) {
    process.stderr.write("plan graph: edge list not found in docs/RECONSTRUCTION.md\n");
    process.exit(1);
}

const edges = new Map();
for (const m of block[1].matchAll(/([0-9]+[A-Za-z]?):\[([^\]]*)\]/g)) {
    edges.set(
        m[1],
        m[2]
            .split(",")
            .map((s) => s.trim())
            .filter(Boolean)
    );
}

const failures = [];
for (const [step, deps] of edges) {
    for (const d of deps) if (!edges.has(d)) failures.push(`${step} depends on ${d}, which is not a step`);
}

// grey/black DFS; report the grey-on-grey path rather than merely "a cycle exists"
const colour = new Map();
const stack = [];
const visit = (n) => {
    if (colour.get(n) === "black") return;
    if (colour.get(n) === "grey") {
        failures.push(`cycle: ${stack.slice(stack.indexOf(n)).concat(n).join(" -> ")}`);
        return;
    }
    colour.set(n, "grey");
    stack.push(n);
    for (const d of edges.get(n) ?? []) visit(d);
    stack.pop();
    colour.set(n, "black");
};
for (const n of edges.keys()) visit(n);

const edgeCount = [...edges.values()].reduce((a, d) => a + d.length, 0);
if (failures.length) {
    process.stderr.write(`plan graph FAILED\n  ${failures.join("\n  ")}\n`);
    process.exit(1);
}
process.stdout.write(`plan graph: ${edges.size} steps, ${edgeCount} edges, acyclic.\n`);
