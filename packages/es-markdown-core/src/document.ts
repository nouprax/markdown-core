import { MarkupDumper } from "./markup-dumper.js";
import { DiagnosticCode, type Diagnostic } from "./model/diagnostic.js";
import type { Document as DocumentValue } from "./model/document.js";
import type { Markup } from "./model/markup.js";
import type { MarkupID } from "./model/markup-id.js";
import type { ParseOptions } from "./parse-options.js";
import { CDocument, decoder, normalizeOptions } from "./runtime/c-document.js";

/** The document type, named the same as the function below so that `Document`
 * is both the annotation and the call that builds one. */
export type Document = DocumentValue;

function diagnosticCode(raw: number): DiagnosticCode {
    if (raw !== 1) throw new Error(`the native parser reported an unknown diagnostic code ${raw}`);
    return DiagnosticCode.directiveAttributes;
}

/**
 * Releases a native parse nobody closed.
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
    readonly series: string;
    readonly identities: Map<number, MarkupID>;
}

/**
 * Interns `rawValue` into `state`'s map, consulting `predecessors` first: an
 * id names the same node across the whole series, so the `MarkupID` a
 * consumer held before an edit is the very object the new tree hands back.
 */
function identity(state: Built, predecessors: ReadonlyMap<number, MarkupID> | null, rawValue: number): MarkupID {
    const existing = state.identities.get(rawValue);
    if (existing) return existing;
    const interned = predecessors?.get(rawValue) ?? { series: state.series, rawValue };
    state.identities.set(rawValue, interned);
    return interned;
}

/**
 * Builds one document value over a freshly taken native parse.
 *
 * Every node is decoded, every time: a document's projection is a function of
 * its text and its options, not of how the caller reached it. An unchanged
 * node still compares equal to its predecessor, which is what a reactive
 * consumer reads — the id/revision pair, not object identity.
 *
 * `predecessors` is the previous revision's interning map, consulted only for
 * the duration of the decode walk and then dropped whole: an id the new tree
 * does not present never enters the successor's map, which is what keeps the
 * map bounded over a long series with no eviction bookkeeping.
 */
function build(state: Built, predecessors: ReadonlyMap<number, MarkupID> | null = null): Document {
    const index = new Map<number, Markup>();
    const diagnostics: readonly Diagnostic[] = state.handle.diagnostics().map((row) => ({
        code: diagnosticCode(row.code),
        scope: row.scope
    }));
    const document = decoder.decodeDocument(state.handle.rootPointer(), {
        ids: (rawValue) => identity(state, predecessors, rawValue),
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

/**
 * Releases `handle` unless `body` returns.
 *
 * A fresh parse belongs to nobody between the call that produced it and the
 * `reclaim` registration `build` ends with, so a throw inside that window is
 * the one way to lose one — and WASM linear memory never gives it back. Both
 * entry points open that window, so both close it here.
 */
function owning<Result>(handle: CDocument, body: () => Result): Result {
    try {
        return body();
    } catch (failure) {
        handle.free();
        throw failure;
    }
}

function edit(state: Built, markdown: string): Document {
    const handle = state.handle.edit(markdown);
    return owning(handle, () =>
        build(
            {
                handle,
                options: state.options,
                series: state.series,
                // Fresh, not copied: the decode walk repopulates it through
                // `identity`, so it holds exactly the ids the new tree
                // presents and nothing a past revision retired.
                identities: new Map()
            },
            state.identities
        )
    );
}

/**
 * Parses `markdown` into a document: the root of the canonical value tree,
 * the owner of the native parse it came from, and the only entry point.
 *
 * There is no session type. A document is created from text and options;
 * `edit` hands it new text and returns the document that text describes.
 * What changed is asked of the new tree itself — a node's id names the same
 * thing across the edit, and its revision says when its content last changed.
 * Options are fixed for a document's whole series — changing what the parser
 * means is a new document, not an edit.
 */
export function Document(markdown: string, options: ParseOptions = {}): Document {
    const normalized = normalizeOptions(options);
    const handle = CDocument.open(markdown, normalized.flags);
    return owning(handle, () =>
        build({
            handle,
            options: normalized.options,
            series: handle.series(),
            identities: new Map()
        })
    );
}
