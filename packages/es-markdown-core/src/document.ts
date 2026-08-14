import { MarkupDumper } from "./markup-dumper.js";
import { children } from "./markup-walker.js";
import { DiagnosticCode, type Diagnostic } from "./model/diagnostic.js";
import type { Document as DocumentValue } from "./model/document.js";
import type { Markup } from "./model/markup.js";
import type { MarkupID } from "./model/markup-id.js";
import type { ParseOptions } from "./parse-options.js";
import type { Scope } from "./values.js";
import type { DocumentValue as DecodedDocument } from "./wire/node-decoder.js";
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
 * reachable, and then it would never run. One registration per document
 * handle, superseded ones included — the native chain cell is refcounted
 * inside the engine, so freeing a superseded handle is an O(1) decrement.
 */
const reclaim = new FinalizationRegistry<CDocument>((handle) => handle.free());

/** Everything one parse settles, so that both mutations and the entry point
 * build the same document value the same way. */
interface Built {
    readonly handle: CDocument;
    readonly options: Readonly<Required<ParseOptions>>;
    readonly series: string;
    /**
     * At most one UTF-16 code unit: a trailing high surrogate the last
     * `append` held back because its low half had not arrived. It prepends
     * to the next append's chunk so a pair split across two appends reaches
     * the encoder whole.
     */
    readonly held: string;
    readonly identities: Map<number, MarkupID>;
    /**
     * Every non-root node of this document by raw id — `node`'s answer, and
     * the next append's value MIRROR: an entry's `revision` is the revision
     * half of the (id, revision) pair, and its value is what an equal pair
     * proves the successor may take whole.
     */
    readonly index: Map<number, Markup>;
}

/** Every built document's internal state, so the value-mirror gate below can
 * reach the native parse behind a public value. Weak: an entry dies with its
 * document. */
const states = new WeakMap<Document, Built>();

/**
 * Interns `rawValue` into `state`'s map, consulting `predecessors` first: an
 * id names the same node across the whole series, so the `MarkupID` a
 * consumer held before a mutation is the very object the new tree hands back.
 */
function identity(state: Built, predecessors: ReadonlyMap<number, MarkupID> | null, rawValue: number): MarkupID {
    const existing = state.identities.get(rawValue);
    if (existing) return existing;
    const interned = predecessors?.get(rawValue) ?? { series: state.series, rawValue };
    state.identities.set(rawValue, interned);
    return interned;
}

/**
 * Enters a reused subtree into the successor's books: every node under
 * `value` — itself included — keeps its index entry and its interned
 * `MarkupID`, so `node` keeps answering and a later full decode re-issues
 * the same id objects. A pure JavaScript walk over already-built values:
 * reuse spends no native call below the (id, revision) pair that proved it.
 *
 * This walk is why per-tick index bookkeeping is O(live nodes) while native
 * decode stays O(changed): the successor's maps start empty, and eviction
 * happens by never entering what the new tree does not present. The eviction
 * itself cannot be skipped — a stale id must not resolve on the successor,
 * which the retired-id tests pin.
 *
 * Deferred: a bounded exact fix needs one of two mechanisms — persistent
 * maps with structural sharing, so the successor derives its maps from the
 * predecessor's in O(changed) instead of rebuilding both, or an
 * engine-provided retirement stream (the deleted delta's ghost), so the
 * predecessor's maps could be carried whole and only the retired ids
 * removed.
 */
function register(state: Built, value: Markup): void {
    const stack: Markup[] = [value];
    while (stack.length > 0) {
        const node = stack.pop()!;
        state.index.set(node.id.rawValue, node);
        state.identities.set(node.id.rawValue, node.id);
        for (const child of children(node)) stack.push(child);
    }
}

/**
 * Builds one document value over a freshly taken native parse.
 *
 * After `Document`, every node is decoded: a full parse settles every
 * position, so no earlier value is known to still be true. After
 * `append`, the decode is PRUNED through `mirror` — the predecessor's index.
 * Wherever the new tree presents an (id, revision) pair the mirror also
 * holds AND the node's extent is unmoved, the predecessor's value is taken
 * whole and the subtree is never visited: a node's revision covers its
 * whole subtree's content (the engine's ledger gate pins exactly this), and
 * appended bytes never move settled bytes. The extent must be matched
 * separately because revision covers content, not position — and an append
 * CAN move a boundary on the hot tail with nothing about content changing:
 * a growing table row's earlier cell gains a trailing padding column the
 * moment the next `|` arrives, and a trailing construct may absorb its
 * terminating newline into its end. The mismatch declines that one node;
 * its children are still tried one by one. Declining is sound in the other
 * direction too: appended bytes only ever grow extents toward the tail, so
 * an ancestor whose extent held vouches for every descendant's. Per append,
 * native decode and value construction are therefore O(changed) — which is
 * the point of appending — while the index bookkeeping behind `node` and
 * the next mirror is O(live nodes) of pure map writes (see `register`).
 *
 * `predecessors` and `mirror` are consulted only for the duration of the
 * walk and then dropped whole: an id the new tree does not present never
 * enters the successor's maps, which is what keeps them bounded over a long
 * chain with no eviction bookkeeping.
 */
function build(
    state: Built,
    predecessors: ReadonlyMap<number, MarkupID> | null = null,
    mirror: ReadonlyMap<number, Markup> | null = null
): Document {
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
                return state.index.get(id.rawValue) ?? null;
            });
            define("append", (chunk: string) => append(state, chunk));
            define("close", () => {
                reclaim.unregister(adopted);
                state.handle.free();
            });
            define("dump", function dump(this: Document): string {
                return MarkupDumper.dump(this);
            });
            return adopted as Document;
        },
        index: state.index,
        ...(mirror === null
            ? {}
            : {
                  reuse: (rawValue: number, revision: number, scope: Scope): Markup | null => {
                      const kept = mirror.get(rawValue);
                      if (kept === undefined || kept.revision !== revision) return null;
                      if (
                          kept.scope.start.line !== scope.start.line ||
                          kept.scope.start.column !== scope.start.column ||
                          kept.scope.end.line !== scope.end.line ||
                          kept.scope.end.column !== scope.end.column
                      ) {
                          return null;
                      }
                      register(state, kept);
                      return kept;
                  }
              })
    });
    states.set(document, state);
    if (!state.handle.released) reclaim.register(document, state.handle, document);
    return document;
}

/**
 * Releases `handle` unless `body` returns.
 *
 * A fresh parse belongs to nobody between the call that produced it and the
 * `reclaim` registration `build` ends with, so a throw inside that window is
 * the one way to lose one — and WASM linear memory never gives it back.
 * Every entry point opens that window, so every entry point closes it here.
 */
function owning<Result>(handle: CDocument, body: () => Result): Result {
    try {
        return body();
    } catch (failure) {
        handle.free();
        throw failure;
    }
}

function append(state: Built, chunk: string): Document {
    // Checked before the boundary's own guard can see it: concatenating the
    // held unit below would coerce a non-string into a string chunk.
    if (typeof chunk !== "string") throw new TypeError("markdown must be a string");
    // Hold-back: each chunk is encoded to UTF-8 on its own, so a surrogate
    // pair split across two appends would otherwise become U+FFFD twice. A
    // high surrogate at the end of the effective chunk may be completed by
    // the next one; it waits here while the rest is appended — possibly
    // nothing, and an empty append is legal and still advances the chain. A
    // trailing LOW surrogate is not a split — nothing held can precede its
    // half, and no later chunk completes it — so it encodes to U+FFFD
    // exactly as `TextEncoder` defines for the caller's own lone surrogate.
    const effective = state.held + chunk;
    const last = effective.charCodeAt(effective.length - 1);
    const sent = last >= 0xd800 && last <= 0xdbff ? effective.length - 1 : effective.length;
    const handle = state.handle.append(effective.slice(0, sent));
    return owning(handle, () =>
        build(
            {
                handle,
                options: state.options,
                series: state.series,
                held: effective.slice(sent),
                identities: new Map(),
                index: new Map()
            },
            state.identities,
            // The predecessor's index is the value mirror: reuse whatever it
            // proves, decode only what changed.
            state.index
        )
    );
}

/**
 * The value-mirror gate's oracle: a fresh, mirror-free decode of
 * `document`'s own native parse, as a bare value tree with no mediators and
 * no interning.
 *
 * This is the only decode that can expose a reuse defect — the pruned tree
 * and this one come from the same native document, so any field on which
 * they disagree was wrongly carried by the mirror. Exists for that gate and
 * deliberately not re-exported through the package index; it throws for a
 * closed or superseded document, because those no longer have a native parse
 * to decode.
 */
export function unprunedDecode(document: Document): DecodedDocument {
    const state = states.get(document);
    if (state === undefined) throw new Error("not a document value this module built");
    return decoder.decodeDocument(state.handle.rootPointer(), {
        ids: (rawValue) => ({ series: state.series, rawValue }),
        adopt: (value) => value as Document,
        index: new Map()
    });
}

/**
 * Parses `markdown` into a document: the root of the canonical value tree,
 * the owner of the native parse it came from, and the only entry point.
 *
 * There is no session type. A document is the live head of a CHAIN, and a
 * chain grows one way: `append` adds bytes at the end, returns the next
 * head, and supersedes its receiver. Replacing the text is a new document
 * (new chain, new series). What changed is asked of the new tree itself — a
 * node's id names the same thing across the mutation, and its revision says
 * when its content last changed. Options are fixed for a chain's whole
 * series — changing what the parser means is a new document, not a mutation.
 */
export function Document(markdown: string, options: ParseOptions = {}): Document {
    const normalized = normalizeOptions(options);
    const handle = CDocument.open(markdown, normalized.flags);
    return owning(handle, () =>
        build({
            handle,
            options: normalized.options,
            series: handle.series(),
            held: "",
            identities: new Map(),
            index: new Map()
        })
    );
}
