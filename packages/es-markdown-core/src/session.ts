import { MarkdownCoreError } from "./common/markdown-core-error.js";
import { SourceLines } from "./common/source-lines.js";
import type { Context, Edit, Slot } from "./markup/context.js";
import type { Document } from "./markup/document.js";
import type { Markup } from "./markup/markup.js";
import type { Position, Scope, TextUnit } from "./markup/values.js";
import { allocate, allocateBytes, decodeMessage } from "./runtime/parser.js";
import { native } from "./runtime/native.js";
import { holdersOf, leadOf } from "./visitor/document-queries.js";
import { readAnswer, type SlotFields } from "./wire/answer-decoder.js";
import { slotKinds, type SlotKind } from "./wire/kinds.js";

const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder();
/** The C `markdown_core_text_unit` of each unit. */
const nativeUnits: { readonly [unit in TextUnit]: number } = { utf8: 1, utf16: 2 };
/** The size of a wasm32 pointer. */
const pointerBytes = 4;
/** The size of a `markdown_core_position`: two i32s. */
const positionBytes = 8;

/** What a session holds in WebAssembly: the engine's session and the
 * contexts its queries made that a slot may still spell from. */
interface Engine {
    session: number;
    readonly contexts: Set<number>;
}

/** Frees what a collected `MarkdownSession` still held. */
const sessions = new FinalizationRegistry<Engine>(release);

/** Frees a context once none of its slots can spell, unless its session
 * already freed it. */
const contexts = new FinalizationRegistry<{ readonly engine: Engine; readonly context: number }>(
    ({ engine, context }) => {
        if (engine.contexts.delete(context)) native.markdown_core_context_free(context);
    }
);

function release(engine: Engine): void {
    for (const context of engine.contexts) native.markdown_core_context_free(context);
    engine.contexts.clear();
    native.markdown_core_session_free(engine.session);
    engine.session = 0;
}

/** Whether `position`'s line and column are integers from 1 to the C
 * `int32_t` bound. */
function valid({ line, column }: Position): boolean {
    const bounded = (value: number): boolean => Number.isInteger(value) && value >= 1 && value <= 0x7fffffff;
    return bounded(line) && bounded(column);
}

/**
 * A text and the document parsed from it, extended together as text is added
 * to its end. The new document continues the previous one: a node that
 * continues an old node keeps its `id`. Every document a session returns is an
 * immutable value that borrows nothing.
 *
 * Until `finalize()`, a document's lookups of a reference, footnote or
 * specimen see only the definitions that start before the inline root
 * asking.
 *
 * The caller ends the text with `finalize()`, which returns the final
 * document; nothing ends a session implicitly. Afterwards `document` and
 * `text` stay readable, and adding text or finalizing again throws
 * `MarkdownCoreError` `sessionFinalized`.
 *
 * The session also answers the queries of an editor: the `context` at a
 * cursor, the `path` of nodes that hold a byte, and the `scope` of a node's
 * fields and its `markers`. Each asks the engine's parse of the session's
 * text, which the session holds, so none parses the text again or takes the
 * source, and the documents stay values.
 *
 * The session's state lives in WebAssembly, with the contexts its queries
 * made that a slot may still spell from: `dispose()` releases it, and a
 * session collected without it is released then. A context without slots is
 * freed when its query answers, and one with slots when its last slot is
 * collected.
 */
export class MarkdownSession {
    /** The unit every scope query's columns count in. */
    readonly unit: TextUnit;
    #engine: Engine;
    #document: Document;

    /** Adds `source` as the session's first text and parses it as text added
     * later is parsed. `unit` is `"utf16"`, the unit of JavaScript strings and
     * editors, by default. Throws `MarkdownCoreError` `allocationFailed` when
     * the parse cannot allocate or the source's UTF-8 exceeds the engine's
     * 1 GiB capacity. */
    constructor(source = "", options?: { readonly unit?: TextUnit }) {
        this.unit = options?.unit ?? "utf16";
        const bytes = utf8Encoder.encode(source);
        let sourcePointer = 0;
        let sessionPointer = 0;
        try {
            sourcePointer = allocateBytes(native, bytes);
            sessionPointer = allocate(native, pointerBytes);
            const message = native.markdown_core_wire_session_new(
                sourcePointer,
                bytes.length,
                nativeUnits[this.unit],
                sessionPointer
            );
            const session = new DataView(native.memory.buffer).getUint32(sessionPointer, true);
            this.#engine = { session, contexts: new Set() };
            this.#document = decodeMessage(native, message, this.unit);
        } finally {
            if (sessionPointer) native.free(sessionPointer);
            if (sourcePointer) native.free(sourcePointer);
        }
        sessions.register(this, this.#engine, this);
    }

    /** The document of the last step. */
    get document(): Document {
        return this.#document;
    }

    /** The session's text. */
    get text(): string {
        return utf8Decoder.decode(this.#text());
    }

    /** Appends `text` and parses the text so far. Throws `MarkdownCoreError`
     * `allocationFailed` when an allocation fails or the text would exceed
     * 1 GiB, and `sessionFinalized` after `finalize()`. */
    append(text: string): Document {
        const session = this.#open();
        const bytes = utf8Encoder.encode(text);
        let textPointer = 0;
        try {
            textPointer = allocateBytes(native, bytes);
            const message = native.markdown_core_wire_session_append(session, textPointer, bytes.length);
            return (this.#document = decodeMessage(native, message, this.unit));
        } finally {
            if (textPointer) native.free(textPointer);
        }
    }

    /** Ends the text and returns the final document, which equals
     * `Document.parse` of `text` in everything but ids. Throws
     * `MarkdownCoreError` `allocationFailed` when an allocation fails, and
     * `sessionFinalized` when the session was already finalized. */
    finalize(): Document {
        const message = native.markdown_core_wire_session_finalize(this.#open());
        return (this.#document = decodeMessage(native, message, this.unit));
    }

    /**
     * The syntactic context at the cursor `position` of `text`, which stands
     * between characters: its column may also stand at its line's end or at
     * the text's end. It counts columns in the session's `unit`.
     *
     * The query parses the text before the cursor and confirms what it leaves
     * open against the session's current `document`, whose engine parse the
     * session holds. Before `finalize()` it confirms as the document resolves:
     * a label sees only the definitions before the inline root asking. The context's slots spell
     * until the session is disposed. Throws `MarkdownCoreError` `outOfBounds`
     * when the position names no place of the text or its line or column is
     * not an integer of at least 1, and `allocationFailed` when an allocation
     * fails.
     */
    context(position: Position): Context {
        if (!valid(position)) throw new MarkdownCoreError("outOfBounds");
        const session = this.#open();
        let positionPointer = 0;
        let contextPointer = 0;
        try {
            positionPointer = this.#position(position);
            contextPointer = allocate(native, pointerBytes);
            const message = native.markdown_core_wire_context(session, positionPointer, contextPointer);
            const context = new DataView(native.memory.buffer).getUint32(contextPointer, true);
            let answer;
            try {
                answer = readAnswer(native, message, (reader) => reader.context());
            } catch (error) {
                if (context) native.markdown_core_context_free(context);
                throw error;
            }
            const { blocks, inlines, slots } = answer;
            // Only a slot spells from the context, so one without slots is
            // freed now, and one with slots when its last slot is collected.
            if (!slots.length) {
                if (context) native.markdown_core_context_free(context);
                return { blocks, inlines, slots: [] };
            }
            const holder = { context };
            this.#engine.contexts.add(context);
            contexts.register(holder, { engine: this.#engine, context });
            return { blocks, inlines, slots: slots.map((fields, index) => this.#slot(fields, holder, index)) };
        } finally {
            if (contextPointer) native.free(contextPointer);
            if (positionPointer) native.free(positionPointer);
        }
    }

    /**
     * Every node of `document` one of whose source ranges holds the byte at
     * `position` of `text`, in canonical walk order, so the root first and the
     * node `Document.nodeAt` finds last, with the column counted in the
     * session's `unit`. None when no node holds the byte or the position names
     * no byte of the text at a scalar boundary. Throws `MarkdownCoreError`
     * `outOfBounds` unless the line and column are integers of at least 1.
     */
    path(position: Position): Markup[] {
        if (!valid(position)) throw new MarkdownCoreError("outOfBounds");
        const offset = new SourceLines(this.#text()).offset(position, this.unit);
        return offset === null ? [] : holdersOf(this.#document, offset);
    }

    /**
     * The source ranges of `node`'s fields of slot kind `field`, one per run
     * of each, in source order: the bytes of each value as written, escapes
     * included, without the quotes, the angle brackets or the sigil around
     * it. Null asks for the fields no slot kind names, such as a title or a
     * literal. A field the parser makes rather than reads, such as a
     * heading's computed anchor, has none.
     *
     * A node's fields and its `markers` divide its own source: its runs less
     * the runs of the nodes it holds. Each query reads the node's own source
     * with the scanners its element read it with, from the engine parse the
     * session holds.
     *
     * `node` is a node of the current `document`, or of an earlier one when
     * the current document holds it unchanged: a node that later text left
     * alone keeps its id and equals its predecessor. Throws `MarkdownCoreError`
     * `outOfBounds` when the current document does not hold `node` or `field`
     * is not a slot kind, and `allocationFailed` when an allocation fails.
     */
    scope(node: Markup, field: SlotKind | null): Scope[] {
        const slot = field === null ? 0 : slotKinds.indexOf(field);
        if (slot < 0) throw new MarkdownCoreError("outOfBounds");
        return this.#parts(node, (session, id, position) =>
            native.markdown_core_wire_field(session, id, position, slot)
        );
    }

    /**
     * The source ranges of `node`'s markers: the rest of its own source, the
     * syntax no field holds, such as the `**` of a strong emphasis, a
     * heading's `#` run, a list item's marker or a fence, and the spaces and
     * line endings between them; see `scope`. Throws `MarkdownCoreError`
     * `outOfBounds` when the current document does not hold `node`, and
     * `allocationFailed` when an allocation fails.
     */
    markers(node: Markup): Scope[] {
        return this.#parts(node, (session, id, position) => native.markdown_core_wire_markers(session, id, position));
    }

    /** Releases the session's WebAssembly state and every context its queries
     * made; a second call does nothing. Its documents stay valid, and every
     * later call, spelling or read of `text` throws an `Error`. */
    dispose(): void {
        sessions.unregister(this);
        if (this.#engine.session) release(this.#engine);
    }

    /** The WebAssembly session, which a disposed session no longer has. */
    #open(): number {
        if (!this.#engine.session) throw new Error("The session is disposed.");
        return this.#engine.session;
    }

    /** The session's text, as its UTF-8 bytes. */
    #text(): Uint8Array {
        const session = this.#open();
        const size = native.markdown_core_session_text_size(session);
        const bytes = allocate(native, Math.max(size, 1));
        try {
            native.markdown_core_session_text(session, bytes);
            return new Uint8Array(native.memory.buffer, bytes, size).slice();
        } finally {
            native.free(bytes);
        }
    }

    /** `position` copied into the module's memory, which the caller frees. */
    #position({ line, column }: Position): number {
        const pointer = allocate(native, positionBytes);
        const view = new DataView(native.memory.buffer);
        view.setInt32(pointer, line, true);
        view.setInt32(pointer + 4, column, true);
        return pointer;
    }

    /** The slot `fields` of the engine's `context`, at `index` among its slots. */
    #slot(fields: SlotFields, holder: { readonly context: number }, index: number): Slot {
        const spell = (value: string): Edit | null => {
            const bytes = utf8Encoder.encode(value);
            // The context outlives no session: a disposed one freed it.
            this.#open();
            let valuePointer = 0;
            try {
                valuePointer = allocateBytes(native, bytes);
                const message = native.markdown_core_wire_spell(holder.context, index, valuePointer, bytes.length);
                return readAnswer(native, message, (answer) => answer.edit(this.unit));
            } finally {
                if (valuePointer) native.free(valuePointer);
            }
        };
        return {
            ...fields,
            spell: ((values: string | readonly string[]) =>
                typeof values === "string" ? spell(values) : values.map(spell)) as Slot["spell"]
        };
    }

    /**
     * A field or marker query of `node` against the engine's parse, which the
     * current `document` copies: the engine finds the node by its id among
     * those that hold the first byte `node` holds. A node that holds no byte
     * owns no source.
     */
    #parts(node: Markup, query: (session: number, id: bigint, position: number) => number): Scope[] {
        const session = this.#open();
        const lead = leadOf(this.#document, node);
        if (lead === null) return [];
        const { start } = new SourceLines(this.#text()).scope(lead, lead + 1, this.unit);
        const positionPointer = this.#position(start);
        try {
            return readAnswer(native, query(session, BigInt(node.id), positionPointer), (answer) => answer.scopes());
        } finally {
            native.free(positionPointer);
        }
    }
}
