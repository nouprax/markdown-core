/** Syntax-directed contracts for deliberately restricted, repeatable productions.
 * These semantic actions are handwritten from the grammar, never learned from
 * parser output. Each action specifies EVERY node and semantic field, including
 * fixed fields. See docs/architecture/benchmark-pair-review.md for the proofs.
 */
import { isDeepStrictEqual } from "node:util";

const marker = "{n:6}";
const separator = "***\n\n";
const registry = new Map();

function syntax(side) {
    const upstream = side === "reference";
    const node = (kind, fields = {}, children = []) => ({
        kind,
        fields: upstream ? fields : { anchor: "null", attributes: "{}", ...fields },
        children
    });
    const virtual = (kind, children = []) => ({ kind, fields: {}, children });
    const text = (literal) => node("Text", { literal });
    const paragraph = (...children) => node("Paragraph", {}, children);
    const prose = (literal) => paragraph(text(literal));
    const inline = (child) => paragraph(text("probe "), child, text(" end"));
    const link = (kind, destination, title, children = []) =>
        node(
            kind,
            upstream
                ? { destination, ...(title !== null ? { title } : {}), dest: { kind: "url", value: destination } }
                : {
                      dest: `url(${JSON.stringify(destination)})`,
                      title: title ?? "null",
                      ...(kind === "Embedded" ? { dimensions: "null" } : {})
                  },
            children
        );
    const list = (
        children,
        { ordered = false, variant = "decimal", delimiter = "period", tight = true, start = "1" } = {}
    ) =>
        node(
            "List",
            upstream
                ? {
                      type: ordered ? "ordered" : "bullet",
                      ...(ordered ? { start, delim: delimiter === "period" ? "period" : "paren" } : {}),
                      tight: String(tight)
                  }
                : {
                      flavor: ordered ? "ordered" : "bullet",
                      start: ordered ? start : "null",
                      variant: ordered ? variant : "null",
                      delimiter: ordered ? delimiter : "null",
                      tight: String(tight)
                  },
            children
        );
    const item = (children, task) =>
        node(
            "ListItem",
            task === undefined
                ? upstream
                    ? { completed: "null" }
                    : { marker: "null" }
                : upstream
                  ? { completed: "true" }
                  : { marker: task },
            children
        );
    const code = (literal) =>
        node(
            "CodeBlock",
            upstream ? { literal } : { info: "null", language: "null", literal, fenced: "true", closed: "true" }
        );
    return { node, virtual, text, paragraph, prose, inline, link, list, item, code };
}

function register(id, dialect, common, action, options = {}) {
    registry.set(`${id}-v2`, {
        id: `${id}-v2`,
        dialect: dialect + separator,
        common: common + separator,
        action,
        ...options
    });
}

for (const [id, token, kind, commonToken, commonKind] of [
    ["run-insertion", "++", "Insertion", "**", "Strong"],
    ["run-mark", "==", "Mark", "**", "Strong"],
    ["run-strike", "~~", "Strikethrough", "**", "Strong"],
    ["run-super", "^", "Superscript", "*", "Emphasis"],
    ["run-sub", "~", "Subscript", "*", "Emphasis"]
])
    register(
        id,
        `probe ${token}body${marker}${token} end\n\n`,
        `probe ${commonToken}body${marker}${commonToken} end\n\n`,
        (s, n, side) => [s.inline(s.node(side === "dialect" ? kind : commonKind, {}, [s.text(`body${n}`)]))]
    );

for (const [id, token, kind, fields] of [
    ["opaque-comment", "%%", "Comment", {}],
    ["opaque-formula", "$", "Formula", { mode: "embedded" }],
    ["opaque-display", "$$", "Formula", { mode: "standalone" }]
])
    register(id, `probe ${token}body ${marker}${token} end\n\n`, `probe \`body ${marker}\` end\n\n`, (s, n, side) => [
        s.inline(
            s.node(side === "dialect" ? kind : "Code", { ...(side === "dialect" ? fields : {}), literal: `body ${n}` })
        )
    ]);

for (const [id, source, kind, newline] of [
    ["leaf-comment", `%%\nbody ${marker}\n%%\n\n`, "Comment", true],
    ["leaf-formula", `$$\nbody ${marker}\n$$\n\n`, "FormulaBlock", false],
    ["leaf-fence", `\`\`\`formula\nbody ${marker}\n\`\`\`\n\n`, "FormulaBlock", false],
    ["leaf-promotion", `$$body ${marker}$$\n\n`, "FormulaBlock", false]
])
    register(id, source, `\`\`\`\nbody ${marker}\n\`\`\`\n\n`, (s, n, side) => [
        side === "dialect" ? s.node(kind, { literal: `body ${n}${newline ? "\n" : ""}` }) : s.code(`body ${n}\n`)
    ]);

register(
    "task-value",
    `- [~] body ${marker}\n\n`,
    `- [x] body ${marker}\n\n`,
    (s, n, side) => [s.list([s.item([s.prose(`body ${n}`)], side === "dialect" ? "~" : "x")])],
    { gfm: true }
);
register(
    "record-span",
    `probe [body ${marker}]{key="value${marker}"} end\n\n`,
    `probe [body ${marker}](/key "value${marker}") end\n\n`,
    (s, n, side) => [
        s.inline(
            side === "dialect"
                ? s.node("Span", { attributes: `{key="value${n}"}` }, [s.text(`body ${n}`)])
                : s.link("Link", "/key", `value${n}`, [s.text(`body ${n}`)])
        )
    ]
);
for (const [id, prefix, kind, twin] of [
    ["cross-link", "", "CrossLink", "Link"],
    ["cross-embed", "!", "CrossEmbedded", "Embedded"]
]) {
    register(
        id,
        `probe ${prefix}[[target${marker}|label${marker}]] end\n\n`,
        `probe ${prefix}[](/target${marker} "label${marker}") end\n\n`,
        (s, n, side) => [
            s.inline(
                side === "dialect"
                    ? s.node(kind, {
                          dest: `cross(path="target${n}",anchor=null)`,
                          label: `label${n}`,
                          ...(prefix ? { dimensions: "null" } : {})
                      })
                    : s.link(twin, `/target${n}`, `label${n}`)
            )
        ]
    );
}
for (const [id, prefix, block] of [
    ["inline-directive", ":", false],
    ["leaf-directive", "::", true]
]) {
    register(
        id,
        `${block ? "" : "probe "}${prefix}note[body ${marker}]${block ? "" : " end"}\n\n`,
        `${block ? `![body ${marker}](/note)` : `probe [![body ${marker}](/label)](/note) end`}\n\n`,
        (s, n, side) => {
            const content = [s.text(`body ${n}`)];
            const result =
                side === "dialect"
                    ? s.node(block ? "DirectiveBlock" : "Directive", { name: "note" }, [
                          s.node("DirectiveLabel", {}, content)
                      ])
                    : block
                      ? s.link("Embedded", "/note", null, content)
                      : s.link("Link", "/note", null, [s.link("Embedded", "/label", null, content)]);
            return [block ? (side === "dialect" ? result : s.paragraph(result)) : s.inline(result)];
        }
    );
}
register("anonymous-container", `::: {}\nbody ${marker}\n:::\n\n`, `> body ${marker}\n\n`, (s, n, side) => [
    s.node(
        side === "dialect" ? "DirectiveBlock" : "Callout",
        side === "dialect" ? { name: "null" } : { variant: "null", collapsed: "null" },
        [s.prose(`body ${n}`)]
    )
]);
register(
    "alpha-list",
    `a) body ${marker}\nb) tail ${marker}\n\n`,
    `1. body ${marker}\n2. tail ${marker}\n\n`,
    (s, n, side) => [
        s.list([s.item([s.prose(`body ${n}`)]), s.item([s.prose(`tail ${n}`)])], {
            ordered: true,
            variant: side === "dialect" ? "alpha(lowercased=true)" : "decimal",
            delimiter: side === "dialect" ? "parenthesis(closed=false)" : "period"
        })
    ]
);
register(
    "loose-definition",
    `term ${marker}\n\n: body ${marker}\n\n`,
    `- term ${marker}\n\n  > body ${marker}\n\n`,
    (s, n, side) => [
        side === "dialect"
            ? s.node("DefinitionList", {}, [
                  s.node("Definition", { compact: "false" }, [
                      s.virtual("DefinitionTerm", [s.text(`term ${n}`)]),
                      s.virtual("DefinitionBody", [s.prose(`body ${n}`)])
                  ])
              ])
            : s.list(
                  [
                      s.item([
                          s.prose(`term ${n}`),
                          s.node("Callout", { variant: "null", collapsed: "null" }, [s.prose(`body ${n}`)])
                      ])
                  ],
                  { tight: false }
              )
    ]
);
register(
    "grid-cell",
    `+----------------+\n| body ${marker}    |\n|                |\n| tail ${marker}    |\n+----------------+\n\n`,
    `- > body ${marker}\n  >\n  > tail ${marker}\n\n`,
    (s, n, side) => {
        const children = [s.prose(`body ${n}`), s.prose(`tail ${n}`)];
        return [
            side === "dialect"
                ? s.node("Table", { columns: "[none:1]" }, [
                      s.virtual("TableHead"),
                      s.virtual("TableBody", [
                          s.node("TableRow", {}, [s.node("TableCell", { rowspan: "1", colspan: "1" }, children)])
                      ]),
                      s.virtual("TableFoot")
                  ])
                : s.list([s.item([s.node("Callout", { variant: "null", collapsed: "null" }, children)])])
        ];
    }
);
register(
    "simple-matrix",
    `Name      Count\n--------  ------\nbody      ${marker}\n\n`,
    `| Name | Count |\n| :--- | :--- |\n| body | ${marker} |\n\n`,
    (s, n, side) => {
        const row = (a, b, header = false) =>
            s.node(
                "TableRow",
                {},
                [a, b].map((v) =>
                    s.node(
                        "TableCell",
                        side === "reference"
                            ? { ...(header ? { align: "left" } : {}), rowspan: "1", colspan: "1" }
                            : { rowspan: "1", colspan: "1" },
                        [s.text(v)]
                    )
                )
            );
        return [
            s.node("Table", side === "reference" ? {} : { columns: "[left:null,left:null]" }, [
                s.virtual("TableHead", [row("Name", "Count", true)]),
                s.virtual("TableBody", [row("body", n)]),
                s.virtual("TableFoot")
            ])
        ];
    },
    { gfm: true }
);

register("class-span", `probe []{.class${marker}} end\n\n`, `probe [](/class${marker}) end\n\n`, (s, n, side) => [
    s.inline(
        side === "dialect" ? s.node("Span", { attributes: `{.class${n}}` }, []) : s.link("Link", `/class${n}`, null, [])
    )
]);
for (const [id, opener, closer, mode] of [
    ["cite-author", "@", "", "authorInText"],
    ["cite-suppress", "-@", "", "suppressAuthor"],
    ["cite-normal", "[@", "]", "normal"]
]) {
    register(
        id,
        `probe ${opener}key${marker}${closer} end\n\n`,
        `probe <https://key${marker}> end\n\n`,
        (s, n, side) => [
            s.inline(
                side === "dialect"
                    ? s.node("Cite", {}, [
                          s.node("Citation", { referent: `bib(key="key${n}",mode=${mode})` }, [
                              s.virtual("CitationPrefix"),
                              s.virtual("CitationSuffix")
                          ])
                      ])
                    : s.link("Link", `https://key${n}`, null, [s.text(`https://key${n}`)])
            )
        ]
    );
}
for (const [id, suffix, label] of [
    ["absent", "", "null"],
    ["empty", "|", ""]
]) {
    for (const [name, prefix, kind, twin] of [
        ["link", "", "CrossLink", "Link"],
        ["embed", "!", "CrossEmbedded", "Embedded"]
    ])
        register(
            `cross-${name}-${id}`,
            `probe ${prefix}[[target${marker}${suffix}]] end\n\n`,
            `probe ${prefix}[](/target${marker}${id === "empty" ? ' ""' : ""}) end\n\n`,
            (s, n, side) => [
                s.inline(
                    side === "dialect"
                        ? s.node(kind, {
                              dest: `cross(path="target${n}",anchor=null)`,
                              label,
                              ...(prefix ? { dimensions: "null" } : {})
                          })
                        : s.link(twin, `/target${n}`, id === "empty" ? "" : null)
                )
            ]
        );
}
for (const local of [false, true])
    register(
        `cross-${local ? "local" : "anchor"}`,
        `probe [[${local ? "" : `target${marker}`}#section${marker}]] end\n\n`,
        `probe [](${local ? "" : `/target${marker}`}#section${marker}) end\n\n`,
        (s, n, side) => [
            s.inline(
                side === "dialect"
                    ? s.node("CrossLink", {
                          dest: `cross(path="${local ? "" : `target${n}`}",anchor="section${n}")`,
                          label: "null"
                      })
                    : s.link("Link", `${local ? "" : `/target${n}`}#section${n}`, null)
            )
        ]
    );

register("named-container", `:::note\nbody ${marker}\n:::\n\n`, `> body ${marker}\n\n`, (s, n, side) => [
    s.node(
        side === "dialect" ? "DirectiveBlock" : "Callout",
        side === "dialect" ? { name: "note" } : { variant: "null", collapsed: "null" },
        [s.prose(`body ${n}`)]
    )
]);
for (const [id, first, second, variant, delimiter, start] of [
    ["upper-list", "(A)", "(B)", "alpha(lowercased=false)", "parenthesis(closed=true)", "1"],
    ["roman-list", "iv.", "v.", "roman(lowercased=true)", "period", "4"],
    ["upper-roman-list", "IV)", "V)", "roman(lowercased=false)", "parenthesis(closed=false)", "4"],
    ["default-list", "#.", "#.", "default", "default", "1"],
    ["enclosed-default-list", "(#)", "(#)", "default", "parenthesis(closed=true)", "1"],
    ["decimal-list", "3.", "4.", "decimal", "period", "3"]
])
    register(
        id,
        `${first} body ${marker}\n${second} tail ${marker}\n\n`,
        `${start}. body ${marker}\n${Number(start) + 1}. tail ${marker}\n\n`,
        (s, n, side) => [
            s.list([s.item([s.prose(`body ${n}`)]), s.item([s.prose(`tail ${n}`)])], {
                ordered: true,
                start,
                variant: side === "dialect" ? variant : "decimal",
                delimiter: side === "dialect" ? delimiter : "period"
            })
        ]
    );

register(
    "empty-directive",
    `probe${marker} :note[] end\n\n`,
    `probe${marker} [![](/label)](/note) end\n\n`,
    (s, n, side) => [
        s.paragraph(
            s.text(`probe${n} `),
            side === "dialect"
                ? s.node("Directive", { name: "note" }, [s.node("DirectiveLabel")])
                : s.link("Link", "/note", null, [s.link("Embedded", "/label", null)]),
            s.text(" end")
        )
    ]
);

// Named definitions have document ownership rather than unit-local ownership.
// XML proves content/order; the HTML witness additionally proves labels/edges.
register(
    "specimen-graph",
    `As (@spec-${marker}) shows.\n\n(@spec-${marker}) Example ${marker}.\n\n`,
    `As [^spec-${marker}] shows.\n\n[^spec-${marker}]: Example ${marker}.\n\n`,
    (s, n, side) => [
        s.paragraph(
            s.text("As "),
            s.node("Cite", {}, [
                s.node(
                    "Citation",
                    side === "reference"
                        ? {}
                        : { referent: `${side === "dialect" ? "specimen" : "footnote"}(id="spec-${n}")` },
                    [s.virtual("CitationPrefix"), s.virtual("CitationSuffix")]
                )
            ]),
            s.text(" shows.")
        )
    ],
    {
        gfm: true,
        unique: true,
        sideList: (s, n, side) =>
            s.node(
                side === "dialect" ? "Specimen" : "Footnote",
                side === "reference" ? {} : { id: `spec-${n}`, ...(side === "dialect" ? { start: "null" } : {}) },
                [s.prose(`Example ${n}.`)]
            ),
        referenceHtml: (values) =>
            values
                .map(
                    (n, i) =>
                        `<p>As <sup class="footnote-ref"><a href="#fn-spec-${n}" id="fnref-spec-${n}" data-footnote-ref>${i + 1}</a></sup> shows.</p>\n<hr />\n`
                )
                .join("") +
            '<section class="footnotes" data-footnotes>\n<ol>\n' +
            values
                .map(
                    (n, i) =>
                        `<li id="fn-spec-${n}">\n<p>Example ${n}. <a href="#fnref-spec-${n}" class="footnote-backref" data-footnote-backref data-footnote-backref-idx="${i + 1}" aria-label="Back to reference ${i + 1}">↩</a></p>\n</li>\n`
                )
                .join("") +
            "</ol>\n</section>\n"
    }
);

export const productionProofs = registry;

function recognizer(template) {
    const escape = (text) => text.replace(/[.*+?^${}()|[\]\\]/gu, "\\$&");
    const parts = template.split(marker);
    return new RegExp(parts.map(escape).join("([0-9]{6})"), "uy");
}
export function productionWorkload(id, dialect, common) {
    const proof = registry.get(id);
    if (!proof) throw new Error(`unknown production proof ${id}`);
    const pattern = recognizer(proof.dialect);
    const values = [];
    let at = 0;
    let transformed = "";
    while (at < dialect.length) {
        pattern.lastIndex = at;
        const found = pattern.exec(dialect);
        if (!found || found.slice(1).some((n) => n !== found[1]))
            throw new Error(`${id}: outside production domain at ${at}`);
        values.push(found[1]);
        transformed += proof.common.replaceAll(marker, found[1]);
        at = pattern.lastIndex;
    }
    if (proof.unique && new Set(values).size !== values.length)
        throw new Error(`${id}: duplicate binding outside domain`);
    if (!values.length || transformed !== common) throw new Error(`${id}: proof mapping does not commute`);
    // The reference recognizer and inverse are checked independently as well.
    const inverse = recognizer(proof.common);
    at = 0;
    for (const n of values) {
        inverse.lastIndex = at;
        const found = inverse.exec(common);
        if (!found || found.slice(1).some((v) => v !== n)) throw new Error(`${id}: inverse mapping failed`);
        at = inverse.lastIndex;
    }
    if (at !== common.length) throw new Error(`${id}: trailing reference input`);
    const canonical = ownership(concrete(proof, "common", values));
    // A reversible template substitution is insufficient: every semantic owner
    // must also correspond node-for-node, in order, across all three actions.
    for (const side of ["dialect", "reference"]) projectOwners(ownership(concrete(proof, side, values)), canonical, id);
    return { ...canonical, values };
}

function concrete(proof, side, values) {
    const s = syntax(side);
    const children = values.flatMap((n) => [...proof.action(s, n, side), s.node("ThematicBreak")]);
    if (proof.sideList) for (const n of values) children.push(proof.sideList(s, n, side));
    return s.node("Document", {}, children);
}

// These groups are AST-printer fields, not independent node owners. Their
// complete contents/partition remain checked by the concrete semantic action.
// In particular, DefinitionTerm/Body and DirectiveLabel are REAL owners.
function ownership(tree) {
    const children = tree.children.flatMap((child) => {
        if (["TableHead", "TableBody", "TableFoot"].includes(child.kind)) return child.children.map(ownership);
        if (["CitationPrefix", "CitationSuffix"].includes(child.kind)) {
            if (child.children.length) throw new Error("unmapped nonempty citation affix");
            return [];
        }
        return [ownership(child)];
    });
    return { kind: tree.kind, fields: tree.fields, children };
}

// Constructor/field encodings come from the handwritten actions, after exact
// checking of every concrete field. Only a pointwise map is allowed: this walk
// cannot introduce/delete an owner or repair a different parent/child relation.
function projectOwners(source, canonical, id, path = "Document") {
    if (source.children.length !== canonical.children.length)
        throw new Error(
            `${id}: ownership mismatch at ${path}: ${source.kind}[${source.children.length}] / ${canonical.kind}[${canonical.children.length}]`
        );
    return {
        kind: canonical.kind,
        fields: canonical.fields,
        children: source.children.map((child, i) => projectOwners(child, canonical.children[i], id, `${path}/${i}`))
    };
}

export function productionTree(id, side, tree, expected, referenceHtml) {
    const proof = registry.get(id);
    if (!proof || !["dialect", "common", "reference"].includes(side)) throw new Error("unknown production or side");
    if (side === "reference" && proof.referenceHtml && referenceHtml !== proof.referenceHtml(expected.values))
        throw new Error(`${id}: reference binding graph mismatch`);
    const wanted = concrete(proof, side, expected.values);
    // Drop printer coordinates only. Default fields are checked, not discarded.
    const clean = (root) => {
        const target = { kind: root.kind, fields: {}, children: [] };
        const stack = [[root, target]];
        while (stack.length) {
            const [a, b] = stack.pop();
            b.fields = Object.fromEntries(
                Object.entries(a.fields).filter(([key]) => !["scope", "sourcepos", "children"].includes(key))
            );
            b.children = a.children.map((child) => ({ kind: child.kind, fields: {}, children: [] }));
            a.children.forEach((child, i) => stack.push([child, b.children[i]]));
        }
        return target;
    };
    const actual = clean(tree);
    if (!isDeepStrictEqual(actual, wanted)) {
        const mismatch = (a, b, path = "Document") => {
            if (a.kind !== b.kind || !isDeepStrictEqual(a.fields, b.fields) || a.children.length !== b.children.length)
                return `${path}: ${JSON.stringify(a.fields)} / expected ${JSON.stringify(b.fields)}; ${a.kind}[${a.children.length}] / ${b.kind}[${b.children.length}]`;
            for (let i = 0; i < a.children.length; i++) {
                const found = mismatch(a.children[i], b.children[i], `${path}/${i}`);
                if (found) return found;
            }
            return null;
        };
        throw new Error(`${id}: ${side} semantic tree mismatch: ${mismatch(actual, wanted)}`);
    }
    return { ...projectOwners(ownership(actual), expected, id), values: expected.values };
}
