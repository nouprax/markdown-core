import type { Document } from "../model/document.js";
import type { FootnoteDefinition, FootnoteReference } from "../model/footnote.js";
import type { Markup } from "../model/markup.js";
import type { MarkupID } from "../model/markup-id.js";
import { ParseError } from "../parse-error.js";
import type { ParseOptions } from "../parse-options.js";
import { CSession } from "../runtime/c-session.js";
import type { RawDelta } from "../runtime/c-session.js";
import type { Commit, Delta } from "./commit.js";
import type { FootnoteInfo } from "./footnote-info.js";
import { relink } from "./relink.js";
import type { ChildSwap } from "./relink.js";
import { ScopeResolver } from "./scope-resolver.js";
import { adopt } from "./snapshot.js";

/**
 * The single mutable owner of one Markdown text and its living AST.
 *
 * Queue edits (`append` is an edit at end-of-text), then `commit`: the
 * session reparses incrementally, keeps node identity wherever content is
 * unchanged, and returns the new snapshot with the exact delta. After any
 * sequence of edits and commits the document is semantically identical to a
 * one-shot `Document.parse` of the same final text.
 *
 * Calls on one session must be externally synchronized (one writer at a
 * time); snapshots are immutable values that survive the session. Commits
 * are transactional: on failure the session stays valid at its previous
 * revision and the commit may be retried (applied edits are retained — the
 * text advances, the tree does not). `close` releases the native session;
 * snapshots, deltas, and materialized scopes remain usable afterwards.
 */
export class MarkupSession {
    /** The session's options, normalized and immutable for its lifetime. */
    readonly options: Readonly<Required<ParseOptions>>;
    /** Per-session random salt; nodes from different sessions never compare
     * equal even when their raw ids collide numerically. */
    readonly lineage: bigint;

    private readonly native: CSession;
    private readonly mirror = new Map<number, Markup>();
    private readonly identities = new Map<number, MarkupID>();
    private readonly rootRawValue: number;
    private resolver: ScopeResolver;
    private currentDocument: Document;
    private closed = false;
    private failed = false;

    constructor(options: ParseOptions = {}) {
        if (options === null || typeof options !== "object") throw new TypeError("options must be an object");
        this.native = new CSession(options);
        try {
            this.options = this.native.options;
            this.lineage = this.native.lineage();
            // The revision-0 root is always an empty document.
            const rootIdentity = this.native.rootIdentity();
            this.rootRawValue = rootIdentity.rawValue;
            const resolver = ScopeResolver.live(this.native);
            this.resolver = resolver;
            const root = adopt(
                {
                    kind: "document",
                    id: this.identity(this.rootRawValue),
                    revision: rootIdentity.revision,
                    content: []
                },
                resolver
            );
            this.mirror.set(this.rootRawValue, root);
            this.currentDocument = root;
        } catch (failure) {
            this.native.free();
            throw failure;
        }
    }

    /** The last committed snapshot; the empty document at revision 0 until
     * the first commit. Snapshots survive `close`. */
    get document(): Document {
        return this.currentDocument;
    }

    /** The revision of the last committed snapshot; 0 before the first
     * commit. */
    get revision(): number {
        this.requireOpen();
        return this.native.revision();
    }

    /** The byte length of the stored text, including uncommitted edits. */
    get length(): number {
        this.requireOpen();
        return this.native.length();
    }

    /** Queues an append of `text`'s UTF-8 bytes at the end of the stored
     * text. Nothing is parsed until `commit`. */
    append(text: string): void {
        if (typeof text !== "string") throw new TypeError("text must be a string");
        this.requireOpen();
        const end = this.native.length();
        this.native.edit(end, end, text);
    }

    /**
     * Queues a replacement of the byte range `[byteStart, byteEnd)` of the
     * stored text with `replacement`'s UTF-8 bytes. An empty range inserts;
     * an empty `replacement` deletes. Offsets refer to the stored text as
     * previously edited; nothing is parsed until `commit`.
     */
    replace(byteStart: number, byteEnd: number, replacement: string): void {
        if (typeof replacement !== "string") throw new TypeError("replacement must be a string");
        // The upper bound keeps the offsets faithful across the WASM
        // boundary: the native size_t parameters are 32-bit, so a larger
        // value would wrap instead of being rejected against the length.
        if (
            !Number.isSafeInteger(byteStart) ||
            !Number.isSafeInteger(byteEnd) ||
            byteStart < 0 ||
            byteStart > byteEnd ||
            byteEnd >= 2 ** 32
        ) {
            throw new RangeError(`invalid edit range [${byteStart}, ${byteEnd})`);
        }
        this.requireOpen();
        this.native.edit(byteStart, byteEnd, replacement);
    }

    /**
     * Reparses the pending text incrementally and returns the new snapshot
     * with its delta. The snapshot shares every unchanged node value with
     * the previous snapshot; the work is proportional to the delta, not the
     * document.
     */
    commit(): Commit {
        this.requireOpen();
        // The previous snapshot's currency ends when the commit starts:
        // detach its resolver before the native tree is replaced, so a
        // not-yet-materialized snapshot can never cache the new revision's
        // positions as its own — a reader either materialized from the
        // still-unchanged tree or takes the documented superseded-snapshot
        // failure.
        const previous = this.resolver;
        previous.detach();
        let deltaPointer: number;
        try {
            deltaPointer = this.native.commit(true);
        } catch (failure) {
            if (failure instanceof ParseError) {
                // The native commit failed transactionally: the tree is
                // unchanged at the previous revision, the previous snapshot
                // becomes current again, and the commit may be retried.
                previous.reattach(this.native);
                throw failure;
            }
            this.failed = true;
            throw failure;
        }
        // Past this point the native tree has advanced: any failure below
        // leaves the mirror out of step, and the session refuses further
        // work.
        try {
            const raw = this.native.readDelta(deltaPointer);
            const delta: Delta = {
                beforeRevision: raw.beforeRevision,
                afterRevision: raw.afterRevision,
                added: raw.added.map((rawValue) => this.identity(rawValue)),
                removed: raw.removed.map((rawValue) => this.identity(rawValue)),
                changed: raw.changed.map((rawValue) => this.identity(rawValue)),
                bubbled: raw.bubbled.map((rawValue) => this.identity(rawValue))
            };
            const resolver = ScopeResolver.live(this.native);
            const touched = new Set<number>([...raw.added, ...raw.changed, ...raw.bubbled]);
            let document: Document;
            if (touched.size === 0) {
                // A pure positional shift: the tree is value-identical, but
                // this snapshot needs its own resolver for the shifted
                // positions.
                const value = this.currentDocument;
                document = adopt(
                    { kind: "document", id: value.id, revision: value.revision, content: value.content },
                    resolver
                );
            } else if (raw.beforeRevision === 0) {
                // First commit: every node is fresh, so one full decode pass
                // skips the by-id lookups and depth ordering of the delta
                // path. This is also what keeps the one-shot `Document.parse`
                // sugar on the v1 performance budget.
                document = this.native.decoder.decodeDocument(this.native.rootPointer(), {
                    ids: (rawValue) => this.identity(rawValue),
                    adopt: (value) => adopt(value, resolver),
                    mirror: this.mirror,
                    touched
                });
            } else {
                document = this.rebuild(raw, resolver);
            }
            for (const rawValue of raw.removed) {
                this.mirror.delete(rawValue);
                this.identities.delete(rawValue);
            }
            this.mirror.set(this.rootRawValue, document);
            this.resolver = resolver;
            this.currentDocument = document;
            return { document, delta };
        } catch (failure) {
            this.failed = true;
            throw failure;
        } finally {
            this.native.deltaFree(deltaPointer);
        }
    }

    /**
     * Applies one delta to the mirror: materialize `added` and `changed`
     * from the native tree, relink `bubbled` from their previous values,
     * children before parents. Untouched siblings are never enumerated
     * through the native boundary, keeping the commit proportional to the
     * delta (plus the pure-JavaScript child-array copies of relinked
     * parents), not to document width.
     */
    private rebuild(raw: RawDelta, resolver: ScopeResolver): Document {
        const decoder = this.native.decoder;
        const rebuilt = new Set<number>([...raw.added, ...raw.changed]);
        const depths = new Map<number, number>();
        const depthOf = (rawValue: number, pointer: number): number => {
            // Memoized: the delta carries every ancestor of a change, so the
            // combined walks stay O(delta), not O(depth) per entry.
            const chain: number[] = [];
            let currentRaw = rawValue;
            let currentPointer = pointer;
            let depth = -1;
            while (true) {
                const cached = depths.get(currentRaw);
                if (cached !== undefined) {
                    depth = cached;
                    break;
                }
                chain.push(currentRaw);
                const parent = this.native.nodeParent(currentPointer);
                if (!parent) break;
                currentPointer = parent;
                currentRaw = decoder.rawId(parent);
            }
            for (let index = chain.length - 1; index >= 0; index -= 1) {
                depth += 1;
                depths.set(chain[index]!, depth);
            }
            return depth;
        };
        const entries = [...raw.added, ...raw.changed, ...raw.bubbled].map((rawValue) => {
            const pointer = this.native.nodeById(rawValue);
            if (!pointer) throw new Error("the delta names a node the session cannot resolve");
            return { rawValue, pointer, depth: depthOf(rawValue, pointer) };
        });
        // Children before parents: a rebuilt or relinked parent assembles
        // its child values from the already-updated mirror.
        entries.sort((a, b) => b.depth - a.depth);
        const swaps = new Map<number, ChildSwap[]>();
        let adopted: Document | null = null;
        for (const entry of entries) {
            const previous = this.mirror.get(entry.rawValue);
            const revision = decoder.revisionOf(entry.pointer);
            let value: Markup;
            if (rebuilt.has(entry.rawValue)) {
                const children = decoder.childPointers(entry.pointer).map((childPointer) => {
                    const child = this.mirror.get(decoder.rawId(childPointer));
                    if (child === undefined || child.revision !== decoder.revisionOf(childPointer)) {
                        throw new Error("the delta omitted a node the session mirror does not carry");
                    }
                    return child;
                });
                value =
                    entry.rawValue === this.rootRawValue
                        ? adopt({ kind: "document", id: this.identity(entry.rawValue), revision, content: children }, resolver)
                        : decoder.decodeValue(entry.pointer, this.identity(entry.rawValue), revision, children);
            } else {
                if (previous === undefined) {
                    throw new Error("the delta bubbled a node the session mirror does not carry");
                }
                const relinked = relink(previous, revision, swaps.get(entry.rawValue) ?? []);
                value =
                    entry.rawValue === this.rootRawValue && relinked.kind === "document"
                        ? adopt(
                              { kind: "document", id: relinked.id, revision: relinked.revision, content: relinked.content },
                              resolver
                          )
                        : relinked;
            }
            this.mirror.set(entry.rawValue, value);
            if (entry.rawValue === this.rootRawValue && value.kind === "document") adopted = value;
            if (previous !== undefined) {
                const parentPointer = this.native.nodeParent(entry.pointer);
                if (parentPointer) {
                    const parentRaw = decoder.rawId(parentPointer);
                    if (!rebuilt.has(parentRaw)) {
                        const list = swaps.get(parentRaw);
                        if (list) list.push({ previous, next: value });
                        else swaps.set(parentRaw, [{ previous, next: value }]);
                    }
                }
            }
        }
        if (adopted === null) throw new Error("the delta did not include the document root");
        return adopted;
    }

    /** The committed snapshot's current value for `id`; null when no node
     * with that identity exists at the committed revision. */
    node(id: MarkupID): Markup | null {
        return id.lineage === this.lineage ? (this.mirror.get(id.rawValue) ?? null) : null;
    }

    /** Answers for the footnote reference or definition with `id` at the
     * committed revision; null when `id` does not name a footnote node of
     * this session. */
    footnote(id: MarkupID): FootnoteInfo | null {
        if (id.lineage !== this.lineage) return null;
        this.requireOpen();
        const raw = this.native.footnoteInfo(id.rawValue);
        if (raw === null) return null;
        return {
            definition: raw.definition === 0 ? null : this.identity(raw.definition),
            number: raw.number === 0 ? null : raw.number,
            referenceOrdinal: raw.referenceOrdinal === 0 ? null : raw.referenceOrdinal,
            referenceCount: raw.referenceCount
        };
    }

    /** The referenced (winning) definitions in first-use order — the order
     * a renderer lists them in. */
    footnotes(): FootnoteDefinition[] {
        this.requireOpen();
        return this.native.footnotes().map((rawValue) => {
            const definition = this.mirror.get(rawValue);
            if (definition?.kind !== "footnoteDefinition")
                throw new Error("footnote index names a non-definition node");
            return definition;
        });
    }

    /** The references that resolve to `definition`, in document order — the
     * renderer's back-reference targets. Empty unless `definition` is a
     * referenced winning definition of this session. */
    references(definition: MarkupID): FootnoteReference[] {
        if (definition.lineage !== this.lineage) return [];
        this.requireOpen();
        return this.native.footnoteReferences(definition.rawValue).map((rawValue) => {
            const reference = this.mirror.get(rawValue);
            if (reference?.kind !== "footnoteReference") throw new Error("footnote index names a non-reference node");
            return reference;
        });
    }

    /**
     * Releases the native session. Idempotent. Snapshots, deltas, and scopes
     * materialized while their snapshot was current remain usable; every
     * other member of this class fails after closing.
     */
    close(): void {
        if (this.closed) return;
        this.closed = true;
        this.resolver.detach();
        this.native.free();
    }

    private identity(rawValue: number): MarkupID {
        const existing = this.identities.get(rawValue);
        if (existing) return existing;
        const created: MarkupID = { lineage: this.lineage, rawValue };
        this.identities.set(rawValue, created);
        return created;
    }

    private requireOpen(): void {
        if (this.closed) throw new Error("the session is closed");
        if (this.failed) throw new Error("the session failed irrecoverably during a commit");
    }
}
