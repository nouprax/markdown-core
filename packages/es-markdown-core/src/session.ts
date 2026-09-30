import type { Document } from "./markup/document.js";
import type { TextUnit } from "./markup/values.js";
import { allocate, allocateBytes, decodeMessage } from "./runtime/parser.js";
import { native } from "./runtime/native.js";

/** One replacement: the text in `[start, end)` becomes `text`. Offsets count in
 * the session's unit, in the text before the batch. */
export interface TextEdit {
    readonly start: number;
    readonly end: number;
    readonly text: string;
}

const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder();
/** The C `markdown_core_text_unit` of each unit. */
const nativeUnits: { readonly [unit in TextUnit]: number } = { utf8: 1, utf16: 2 };
/** The size of a wasm32 `size_t` and a pointer. */
const wordBytes = 4;

/** Frees the session a collected `MarkdownSession` still held. */
const sessions = new FinalizationRegistry<number>((session) => native.markdown_core_session_free(session));

/**
 * A text and the document parsed from it, changed together by each edit. The
 * new document continues the previous one: a node that continues an old node
 * keeps its `id`. Every document a session returns is an immutable value that
 * borrows nothing.
 *
 * The session's state lives in WebAssembly: `dispose()` releases it,
 * and a session collected without it is released then.
 */
export class MarkdownSession {
    /** The unit edit offsets and every scope query's columns count in. */
    readonly unit: TextUnit;
    #session: number;
    #document: Document;

    /** Parses `source` as `Document.parse` does. `unit` is `"utf16"`, the unit
     * of JavaScript strings and editors, by default. Throws `MarkdownCoreError`
     * `allocationFailed` when the parse cannot allocate or the source's UTF-8
     * exceeds the engine's 1 GiB capacity. */
    constructor(source = "", options?: { readonly unit?: TextUnit }) {
        this.unit = options?.unit ?? "utf16";
        const bytes = utf8Encoder.encode(source);
        let sourcePointer = 0;
        let sessionPointer = 0;
        try {
            sourcePointer = allocateBytes(native, bytes);
            sessionPointer = allocate(native, wordBytes);
            const message = native.markdown_core_wire_session_new(
                sourcePointer,
                bytes.length,
                nativeUnits[this.unit],
                sessionPointer
            );
            this.#session = new DataView(native.memory.buffer).getUint32(sessionPointer, true);
            this.#document = decodeMessage(native, message, this.unit);
        } finally {
            if (sessionPointer) native.free(sessionPointer);
            if (sourcePointer) native.free(sourcePointer);
        }
        sessions.register(this, this.#session, this);
    }

    /** The document of the last step. */
    get document(): Document {
        return this.#document;
    }

    /** The session's text. */
    get text(): string {
        const session = this.#open();
        const size = native.markdown_core_session_text_size(session);
        const bytes = allocate(native, Math.max(size, 1));
        try {
            native.markdown_core_session_text(session, bytes);
            return utf8Decoder.decode(new Uint8Array(native.memory.buffer, bytes, size));
        } finally {
            native.free(bytes);
        }
    }

    /** Applies disjoint edits, listed in any order, to the text and parses it
     * once; two edits at one offset apply in the order listed. Throws
     * `MarkdownCoreError` `outOfBounds` when an edit's start is after its end,
     * its end is past the text, two edits overlap, or, in UTF-16, an offset
     * falls between the two units of one scalar; `allocationFailed` when an
     * allocation fails or the text would exceed 1 GiB. */
    edit(edits: readonly TextEdit[]): Document {
        const session = this.#open();
        const texts = edits.map((edit) => utf8Encoder.encode(edit.text));
        const joined = new Uint8Array(texts.reduce((size, text) => size + text.length, 0));
        let offset = 0;
        for (const text of texts) {
            joined.set(text, offset);
            offset += text.length;
        }
        let editsPointer = 0;
        let textsPointer = 0;
        try {
            editsPointer = allocate(native, Math.max(edits.length * 3 * wordBytes, 1));
            textsPointer = allocateBytes(native, joined);
            const words = new Uint32Array(native.memory.buffer, editsPointer, edits.length * 3);
            edits.forEach((edit, index) => {
                words.set([edit.start, edit.end, texts[index]!.length], index * 3);
            });
            const message = native.markdown_core_wire_session_edit(session, editsPointer, edits.length, textsPointer);
            return (this.#document = decodeMessage(native, message, this.unit));
        } finally {
            if (textsPointer) native.free(textsPointer);
            if (editsPointer) native.free(editsPointer);
        }
    }

    /** Appends `text`: the edit at the end of the text. */
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

    /** Releases the session's WebAssembly state; a second call does nothing.
     * Its documents stay valid, and every later call or read of `text` throws
     * an `Error`. */
    dispose(): void {
        sessions.unregister(this);
        native.markdown_core_session_free(this.#session);
        this.#session = 0;
    }

    /** The WebAssembly session, which a disposed session no longer has. */
    #open(): number {
        if (!this.#session) throw new Error("The session is disposed.");
        return this.#session;
    }
}
