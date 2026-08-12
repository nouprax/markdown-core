import { MarkupDumper } from "./markup-dumper.js";
import type { Commit, Delta, Diff, DiffParts } from "./model/commit.js";
import { DiagnosticCode, type Diagnostic } from "./model/diagnostic.js";
import type { Document as DocumentValue } from "./model/document.js";
import type { Markup } from "./model/markup.js";
import type { MarkupID } from "./model/markup-id.js";
import type { ParseOptions } from "./parse-options.js";
import { CDocument, decoder, normalizeOptions } from "./runtime/c-document.js";

/** The document type, named the same as the function below so that `Document`
 * is both the annotation and the call that builds one. */
export type Document = DocumentValue;

const diffPartFlags = { value: 1, text: 2, children: 4, descendant: 8 } as const;

function diffParts(raw: number): DiffParts {
    return {
        retired: raw === 0,
        value: (raw & diffPartFlags.value) !== 0,
        text: (raw & diffPartFlags.text) !== 0,
        children: (raw & diffPartFlags.children) !== 0,
        descendant: (raw & diffPartFlags.descendant) !== 0
    };
}

function diagnosticCode(raw: number): DiagnosticCode {
    if (raw !== 1) throw new Error(`the native parser reported an unknown diagnostic code ${raw}`);
    return DiagnosticCode.directiveAttributes;
}

/**
 * Releases a native parse that its document never handed on and nobody
 * closed.
 *
 * `close` remains the way to release promptly; this is what keeps a dropped
 * document from leaking, and it is never a reason not to close one. The
 * registry holds the {@link CDocument} and never the value it belongs to: a
 * cleanup that could reach its own document would keep that document
 * reachable, and then it would never run.
 */
const reclaim = new FinalizationRegistry<CDocument>((handle) => handle.free());

/** Everything one parse settles, so that both entry points build the same
 * document value the same way. */
interface Built {
    readonly handle: CDocument;
    readonly options: Readonly<Required<ParseOptions>>;
    readonly series: bigint;
    readonly identities: Map<number, MarkupID>;
}

function identity(state: Built, rawValue: number): MarkupID {
    const existing = state.identities.get(rawValue);
    if (existing) return existing;
    const created: MarkupID = { series: state.series, rawValue };
    state.identities.set(rawValue, created);
    return created;
}

/**
 * Builds one document value over a freshly taken native parse.
 *
 * Every node is decoded, every time: a document's projection is a function of
 * its text and its options, not of how the caller reached it. An unchanged
 * node still compares equal to its predecessor, which is what a reactive
 * consumer reads — the id/revision pair, not object identity.
 */
function build(state: Built): Document {
    const index = new Map<number, Markup>();
    const diagnostics: readonly Diagnostic[] = state.handle.diagnostics().map((row) => ({
        code: diagnosticCode(row.code),
        scope: row.scope
    }));
    const document = decoder.decodeDocument(state.handle.rootPointer(), {
        ids: (rawValue) => identity(state, rawValue),
        adopt: (value) => {
            const adopted = {
                kind: value.kind,
                id: value.id,
                revision: value.revision,
                scope: value.scope,
                content: value.content
            };
            // The mediators are non-enumerable, so the document stays a plain
            // value for enumeration, structural comparison, and JSON.
            const define = (name: string, method: unknown): void => {
                Object.defineProperty(adopted, name, { enumerable: false, value: method });
            };
            define("options", state.options);
            define("diagnostics", diagnostics);
            define("node", (id: MarkupID) => {
                if (id.series !== state.series) return null;
                if (id.rawValue === value.id.rawValue) return adopted as Document;
                return index.get(id.rawValue) ?? null;
            });
            define("edit", (markdown: string) => edit(state, markdown));
            define("close", () => {
                reclaim.unregister(adopted);
                state.handle.free();
            });
            define("dump", function dump(this: Document): string {
                return MarkupDumper.dump(this);
            });
            return adopted as Document;
        },
        index
    });
    if (!state.handle.released) reclaim.register(document, state.handle, document);
    return document;
}

function edit(state: Built, markdown: string): Commit {
    const { document: handle, delta: deltaPointer } = state.handle.edit(markdown);
    try {
        const raw = handle.readDelta(deltaPointer);
        const delta: Delta = {
            beforeRevision: raw.beforeRevision,
            afterRevision: raw.afterRevision,
            diffs: raw.diffs.map((row): Diff => ({
                markup: identity(state, row.rawValue),
                parts: diffParts(row.parts)
            }))
        };
        const successor: Built = {
            handle,
            options: state.options,
            series: state.series,
            // Identity interning carries over: an id names the same node
            // across the whole series, so the `MarkupID` a consumer held
            // before the edit is the one the new tree hands back.
            identities: new Map(state.identities)
        };
        for (const row of raw.diffs) {
            if (row.parts === 0) successor.identities.delete(row.rawValue);
        }
        return { document: build(successor), delta };
    } finally {
        handle.deltaFree(deltaPointer);
    }
}

/**
 * Parses `markdown` into a document: the root of the canonical value tree,
 * the owner of the native parse it came from, and the only entry point.
 *
 * There is no session type. A document is created from text and options;
 * `edit` hands it new text and returns the next document with the delta
 * between them. Options are fixed for a document's whole series — changing
 * what the parser means is a new document, not an edit, and there is no
 * delta to be had across it.
 */
export function Document(markdown: string, options: ParseOptions = {}): Document {
    const normalized = normalizeOptions(options);
    const handle = CDocument.open(markdown, normalized.flags);
    try {
        return build({
            handle,
            options: normalized.options,
            series: handle.series(),
            identities: new Map()
        });
    } catch (failure) {
        handle.free();
        throw failure;
    }
}
