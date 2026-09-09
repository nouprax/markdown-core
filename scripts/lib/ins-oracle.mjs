import { createHash } from "node:crypto";
import MarkdownIt from "markdown-it";
import insPlugin from "markdown-it-ins";

// HTML is needed to test token opacity. Other markdown-it defaults (including
// its nesting guard) stay pinned; this adapter never enables a product switch.
export const oracle = new MarkdownIt({ html: true }).use(insPlugin);

const tokenKinds = {
    paragraph: "Paragraph",
    heading: "Heading",
    blockquote: "Callout",
    bullet_list: "List",
    ordered_list: "List",
    list_item: "ListItem",
    em: "Emphasis",
    strong: "Strong",
    s: "Strikethrough",
    ins: "Insertion",
    link: "Link",
    table: "Table",
    thead: "TableHead",
    tbody: "TableBody",
    tr: "TableRow",
    th: "TableCell",
    td: "TableCell"
};
const leafKinds = {
    text: "Text",
    code_inline: "Code",
    code_block: "CodeBlock",
    fence: "CodeBlock",
    html_inline: "HTML",
    html_block: "HTMLBlock",
    softbreak: "SoftBreak",
    hardbreak: "LineBreak",
    hr: "ThematicBreak"
};
const literalKinds = new Set(["Text", "Code", "CodeBlock", "HTML", "HTMLBlock", "Comment", "Formula", "FormulaBlock"]);
const canonicalKinds = new Set([
    ...Object.values(tokenKinds),
    ...Object.values(leafKinds),
    "TableFoot",
    "Document",
    "Media",
    "CrossLink",
    "CrossEmbedded",
    "Mark",
    "Cite",
    "Citation",
    "CitationPrefix",
    "CitationSuffix",
    "Footnote",
    "Comment",
    "Formula",
    "FormulaBlock",
    "Directive",
    "DirectiveLabel",
    "Title",
    "Label"
]);

function append(children, node) {
    if (node.kind === "Text" && node.literal === "") return;
    const last = children.at(-1);
    if (node.kind === "Text" && last?.kind === "Text") last.literal += node.literal;
    else children.push(node);
}

// Compare insertion boundaries relative to the complete ordered token tree.
// Scopes and unrelated scalar attributes are outside this projection, while
// opaque literal bodies and all container boundaries remain observable.
export function fromTokens(tokens) {
    const root = { kind: "Document", children: [] };
    const stack = [{ node: root, type: null }];
    for (const token of tokens) {
        if (token.type === "inline") {
            for (const node of fromTokens(token.children ?? []).children) append(stack.at(-1).node.children, node);
        } else if (token.nesting === 1) {
            const type = token.type.replace(/_open$/, "");
            const kind = tokenKinds[type];
            if (!kind) throw new Error(`unmapped opening token: ${token.type}`);
            const node = { kind, children: [] };
            append(stack.at(-1).node.children, node);
            stack.push({ node, type });
        } else if (token.nesting === -1) {
            const type = token.type.replace(/_close$/, "");
            if (stack.length === 1 || stack.at(-1).type !== type) throw new Error(`unbalanced token: ${token.type}`);
            stack.pop();
        } else if (token.type === "image") {
            append(stack.at(-1).node.children, { kind: "Media", children: fromTokens(token.children ?? []).children });
        } else {
            const kind = leafKinds[token.type];
            if (!kind) throw new Error(`unmapped token: ${token.type}`);
            append(
                stack.at(-1).node.children,
                literalKinds.has(kind) ? { kind, literal: token.content, children: [] } : { kind, children: [] }
            );
        }
    }
    if (stack.length !== 1) throw new Error("unclosed oracle token");
    return root;
}

export function fromCanonical(node) {
    if (!canonicalKinds.has(node.kind)) throw new Error(`unmapped canonical kind: ${node.kind}`);
    const children = [];
    for (const child of node.children) {
        // Empty named table arrays are model fields, not token containers.
        if (["TableHead", "TableBody", "TableFoot"].includes(child.kind) && child.children.length === 0) continue;
        append(children, fromCanonical(child));
    }
    return literalKinds.has(node.kind)
        ? { kind: node.kind, literal: node.fields.literal, children }
        : { kind: node.kind, children };
}

export function assertCanaries() {
    const parsed = oracle.parse("++inserted++", {});
    const tokens = parsed.flatMap((token) => token.children ?? []);
    if (
        tokens.filter((token) => token.type === "ins_open").length !== 1 ||
        tokens.filter((token) => token.type === "ins_close").length !== 1
    )
        throw new Error("insertion oracle is inactive");
    const projected = fromTokens(parsed);
    if (
        JSON.stringify(projected.children) !==
        JSON.stringify([
            {
                kind: "Paragraph",
                children: [{ kind: "Insertion", children: [{ kind: "Text", literal: "inserted", children: [] }] }]
            }
        ])
    )
        throw new Error("insertion projection canary failed");
    const nested = fromTokens(oracle.parse("+++a+++++b++", {}));
    const text = (literal) => ({ kind: "Text", literal, children: [] });
    const inserted = (literal) => ({ kind: "Insertion", children: [text(literal)] });
    if (
        JSON.stringify(nested.children[0].children) !==
        JSON.stringify([text("+"), inserted("a"), text("+"), inserted("b")])
    )
        throw new Error("projection lost odd-run placement");
    for (const input of ["`++code++`", "\\+\\+escaped++", '<b title="++attribute++">literal</b>']) {
        if (oracle.parse(input, {}).some((token) => token.children?.some((child) => child.type === "ins_open")))
            throw new Error("opaque canary opened an insertion");
    }
}

export function validatePolicy(policy, cases) {
    if (policy.schemaVersion !== 1 || !Array.isArray(policy.baselineGaps) || !Array.isArray(policy.expectedDivergences))
        throw new Error("invalid insertion policy");
    if (policy.baselineGaps.length !== 0) throw new Error("I1 must leave no insertion baseline gaps");
    if (!Array.isArray(cases) || !cases.length) throw new Error("empty insertion corpus");
    if (policy.corpusDigest !== digest(JSON.stringify(cases)))
        throw new Error("corpus changed without an evidence review");
    const ids = new Set(),
        inputs = new Set();
    for (const testCase of cases) {
        if (
            !testCase.id ||
            ids.has(testCase.id) ||
            typeof testCase.input !== "string" ||
            !testCase.input ||
            inputs.has(testCase.input)
        )
            throw new Error("duplicate or invalid corpus case");
        if (!["upstream", "module", "composition"].includes(testCase.source))
            throw new Error("unknown corpus provenance");
        ids.add(testCase.id);
        inputs.add(testCase.input);
    }
    const entries = new Map();
    for (const entry of policy.expectedDivergences) {
        if (
            !ids.has(entry.id) ||
            entries.has(entry.id) ||
            !entry.reason?.trim() ||
            cases.find((testCase) => testCase.id === entry.id).input !== entry.input ||
            !/^[0-9a-f]{64}$/.test(entry.oracleDigest) ||
            !/^[0-9a-f]{64}$/.test(entry.markdownCoreDigest)
        )
            throw new Error(`invalid or stale divergence: ${entry.id}`);
        entries.set(entry.id, entry);
    }
    return entries;
}

function digest(value) {
    return createHash("sha256").update(value).digest("hex");
}

export function verifyComparison(entry, expected, actual) {
    if (expected === actual) {
        if (entry) throw new Error("divergence now agrees; remove its policy entry");
        return false;
    }
    if (entry && entry.oracleDigest === digest(expected) && entry.markdownCoreDigest === digest(actual)) return true;
    throw new Error(
        `unregistered or changed divergence\n  oracleDigest=${digest(expected)}\n  markdownCoreDigest=${digest(actual)}\n  oracle=${expected}\n  product=${actual}`
    );
}
