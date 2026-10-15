import type { Document } from "./markup/document.js";
import type { TextUnit } from "./markup/values.js";
import { allocate, allocateBytes, decodeMessage } from "./runtime/parser.js";
import { native } from "./runtime/native.js";

const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder();
/** The C `markdown_core_text_unit` of each unit. */
const nativeUnits: { readonly [unit in TextUnit]: number } = { utf8: 1, utf16: 2 };
/** The size of a wasm32 pointer. */
const pointerBytes = 4;

/** Frees the session a collected `MarkdownSession` still held. */
const sessions = new FinalizationRegistry<number>((session) => native.markdown_core_session_free(session));

/**
 * A text and the document parsed from it, extended together by each append.
 * The new document continues the previous one: a node that continues an old
 * node keeps its `id`. Every document a session returns is an immutable value
 * that borrows nothing.
 *
 * The session's state lives in WebAssembly: `dispose()` releases it,
 * and a session collected without it is released then.
 */
export class MarkdownSession {
    /** The unit every scope query's columns count in. */
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
            sessionPointer = allocate(native, pointerBytes);
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

    /** Appends `text` and parses the result. Throws `MarkdownCoreError`
     * `allocationFailed` when an allocation fails or the text would exceed
     * 1 GiB. */
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
