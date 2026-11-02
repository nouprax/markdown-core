/**
 * The language engine (docs/plans/2026-10-23-lsp-completion-engine.md,
 * section 4): the semantics of the Language Server Protocol 3.17 over
 * Markdown Core, called in process. Each method of `Server` is one protocol
 * method, and answers as the C engine `markdown-core-lsp` does, so this
 * server, the other bindings and the JSON-RPC adapter `markdown-core-server`
 * give the same answers.
 *
 * @module
 */
import type { TextUnit } from "./markup/values.js";
import { native } from "./runtime/native.js";

const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder();

/** The C `markdown_core_text_unit` of each unit. */
const nativeUnits: { readonly [unit in TextUnit]: number } = { utf8: 1, utf16: 2 };
/** The size of a wasm32 pointer, and of a `size_t`. */
const pointerBytes = 4;
/** The size of a `markdown_core_lsp_position`: two u32s. */
const positionBytes = 8;
/** The size of a `markdown_core_lsp_edit`: a scope of four u32s, a pointer and a size. */
const editBytes = 24;
/** The size of a `markdown_core_lsp_capabilities`: a unit, then a pointer and a size. */
const capabilitiesBytes = 12;
/** The bound of a protocol number, which the engine reads as a `uint32_t`. */
const bound = 0xffffffff;

/**
 * Where a server is in the protocol's lifecycle: `"fresh"` until initialize,
 * `"active"` until shutdown, and `"halted"` afterwards.
 */
export type Phase = "fresh" | "active" | "halted";

const phases: readonly Phase[] = ["fresh", "active", "halted"];

/** What a server answers initialize with. */
export interface Capabilities {
    /** The position encoding every `Position` counts in. */
    readonly unit: TextUnit;
    /** The completion trigger characters, one per character: the parser's
     * slot cues, which are ASCII. */
    readonly cues: string;
}

/**
 * A protocol position: `line` and `character` counted from 0, in the
 * negotiated `Capabilities.unit`. A character past its line's end stands at
 * the line's end, one inside a character stands at the character's start,
 * and a line past the text's last stands at the text's end.
 */
export interface Position {
    readonly line: number;
    readonly character: number;
}

/** A protocol range: from `start` up to `end`, which it does not hold. */
export interface Scope {
    readonly start: Position;
    readonly end: Position;
}

/** A content change of didChange: `text` replaces `scope`, or the whole text
 * when it names none. */
export interface Edit {
    readonly scope?: Scope | null;
    readonly text: string;
}

/**
 * The answer to one completion request: the `version` of the document it was
 * computed for, and the `count` of its items. The items come with the
 * providers of the plan's section 5.
 */
export interface Completions {
    readonly version: number;
    readonly count: number;
}

/**
 * Why a server method failed, each the protocol's error code:
 * - `"notInitialized"` (-32002): the call came before initialize;
 * - `"invalidRequest"` (-32600): the call is not one the server's phase
 *   takes: a second initialize, or any call after shutdown;
 * - `"invalidParams"` (-32602): the call names a document that is not open,
 *   an edit whose start is after its end, or a number the protocol does not
 *   hold there: a version that is not an int32, or a line or character that
 *   is not a uint32;
 * - `"internalError"` (-32603): an allocation failed, or a text exceeds what
 *   the parser can hold.
 */
export type ServerCode = "notInitialized" | "invalidRequest" | "invalidParams" | "internalError";

const codes: { readonly [code in ServerCode]: number } = {
    notInitialized: -32002,
    invalidRequest: -32600,
    invalidParams: -32602,
    internalError: -32603
};

/** The one error a server method throws, carrying its code. */
export class ServerError extends Error {
    readonly code: ServerCode;
    /** The code's number in the protocol. */
    readonly value: number;

    constructor(code: ServerCode) {
        super(code);
        this.name = "ServerError";
        this.code = code;
        this.value = codes[code];
    }
}

/** Throws the failure `status` names, if any. */
function pass(status: number): void {
    if (status === 0) return;
    const code = (Object.keys(codes) as ServerCode[]).find((name) => codes[name] === status);
    throw new ServerError(code ?? "internalError");
}

/** What a server holds in WebAssembly. */
interface Engine {
    server: number;
}

/** Frees what a collected `Server` still held. */
const servers = new FinalizationRegistry<Engine>(release);

function release(engine: Engine): void {
    native.markdown_core_lsp_server_free(engine.server);
    engine.server = 0;
}

/** `size` bytes of the module's memory, at least one. */
function reserve(size: number): number {
    const pointer = native.malloc(Math.max(size, 1));
    if (!pointer) throw new ServerError("internalError");
    return pointer;
}

/** `bytes` copied into the module's memory. */
function place(bytes: Uint8Array): number {
    const pointer = reserve(bytes.length);
    new Uint8Array(native.memory.buffer, pointer, bytes.length).set(bytes);
    return pointer;
}

/** Whether `value` is an integer from `min` to `max`. */
function within(value: number, min: number, max: number): boolean {
    return Number.isInteger(value) && value >= min && value <= max;
}

function checkPosition({ line, character }: Position): void {
    if (!within(line, 0, bound) || !within(character, 0, bound)) throw new ServerError("invalidParams");
}

function checkVersion(version: number): void {
    if (!within(version, -0x80000000, 0x7fffffff)) throw new ServerError("invalidParams");
}

/**
 * A server of the open documents of one client. It does no I/O: the host
 * hands it every text. It has one writer at a time.
 *
 * A method that fails throws `ServerError` and changes nothing, except an
 * `update` that fails with `"internalError"`, which closes the document.
 *
 * The server's state lives in WebAssembly: `dispose()` releases it, and a
 * server collected without it is released then.
 */
export class Server {
    #engine: Engine;

    /** A new server, in the `"fresh"` phase. Throws `ServerError`
     * `"internalError"` when it cannot be allocated. */
    constructor() {
        const pointer = reserve(pointerBytes);
        try {
            pass(native.markdown_core_lsp_server_new(pointer));
            this.#engine = { server: new DataView(native.memory.buffer).getUint32(pointer, true) };
        } finally {
            native.free(pointer);
        }
        servers.register(this, this.#engine, this);
    }

    /** Where the server is in the protocol's lifecycle. A host that reads
     * `exit` ends with code 0 when it is `"halted"`, and 1 otherwise. */
    get phase(): Phase {
        return phases[native.markdown_core_lsp_server_phase(this.#open())] ?? "halted";
    }

    /**
     * The initialize request. `units` are the position encodings the client
     * offers, which the protocol's `general.positionEncodings` lists. The
     * server becomes `"active"`.
     *
     * Returns the position encoding, UTF-8 when the client offers it and
     * UTF-16 otherwise, and the completion trigger characters. Throws
     * `ServerError` `"invalidRequest"` after the first initialize.
     */
    initialize(units: readonly TextUnit[]): Capabilities {
        const server = this.#open();
        let list = 0;
        let answer = 0;
        try {
            list = reserve(units.length * 4);
            answer = reserve(capabilitiesBytes);
            const view = new DataView(native.memory.buffer);
            units.forEach((unit, index) => view.setUint32(list + index * 4, nativeUnits[unit], true));
            pass(native.markdown_core_lsp_initialize(server, list, units.length, answer));
            const result = new DataView(native.memory.buffer);
            const data = result.getUint32(answer + 4, true);
            const length = result.getUint32(answer + 8, true);
            return {
                unit: result.getUint32(answer, true) === nativeUnits.utf8 ? "utf8" : "utf16",
                cues: utf8Decoder.decode(new Uint8Array(native.memory.buffer, data, length))
            };
        } finally {
            if (answer) native.free(answer);
            if (list) native.free(list);
        }
    }

    /** The shutdown request. The server becomes `"halted"` and takes no more
     * calls. */
    shutdown(): void {
        pass(native.markdown_core_lsp_shutdown(this.#open()));
    }

    /**
     * The didOpen notification: the document at `uri` holds `text` at
     * `version`. A document opened again replaces the one open at its URI.
     * The server parses the text into the document's session, which each
     * later change edits.
     */
    open(uri: string, version: number, text: string): void {
        const server = this.#open();
        checkVersion(version);
        const name = utf8Encoder.encode(uri);
        const bytes = utf8Encoder.encode(text);
        let namePointer = 0;
        let textPointer = 0;
        try {
            namePointer = place(name);
            textPointer = place(bytes);
            pass(native.markdown_core_lsp_open(server, namePointer, name.length, version, textPointer, bytes.length));
        } finally {
            if (textPointer) native.free(textPointer);
            if (namePointer) native.free(namePointer);
        }
    }

    /**
     * The didChange notification: `edits`, the content changes, applied to
     * the document in order, each to the text the one before it made, which
     * give it `version`.
     *
     * Throws `ServerError` `"invalidParams"` when the document is not open
     * or an edit's start is after its end, which every edit is checked for
     * before the first applies; the document then keeps its text and
     * version. `"internalError"` when an edit cannot allocate, and then the
     * server closes the document, since it holds some of the edits and
     * cannot tell the client's text.
     */
    update(uri: string, version: number, edits: readonly Edit[]): void {
        const server = this.#open();
        checkVersion(version);
        for (const { scope } of edits) {
            if (scope) {
                checkPosition(scope.start);
                checkPosition(scope.end);
            }
        }
        const name = utf8Encoder.encode(uri);
        const texts = edits.map(({ text }) => utf8Encoder.encode(text));
        const joined = new Uint8Array(texts.reduce((total, bytes) => total + bytes.length, 0));
        let namePointer = 0;
        let textPointer = 0;
        let list = 0;
        try {
            namePointer = place(name);
            textPointer = reserve(joined.length);
            list = reserve(edits.length * editBytes);
            const memory = new Uint8Array(native.memory.buffer);
            const view = new DataView(native.memory.buffer);
            let start = 0;
            edits.forEach(({ scope }, index) => {
                const bytes = texts[index] ?? new Uint8Array();
                memory.set(bytes, textPointer + start);
                const at = list + index * editBytes;
                // A change with no range replaces the whole text.
                view.setUint32(at, scope ? scope.start.line : 0, true);
                view.setUint32(at + 4, scope ? scope.start.character : 0, true);
                view.setUint32(at + 8, scope ? scope.end.line : bound, true);
                view.setUint32(at + 12, scope ? scope.end.character : bound, true);
                view.setUint32(at + 16, textPointer + start, true);
                view.setUint32(at + 20, bytes.length, true);
                start += bytes.length;
            });
            pass(native.markdown_core_lsp_update(server, namePointer, name.length, version, list, edits.length));
        } finally {
            if (list) native.free(list);
            if (textPointer) native.free(textPointer);
            if (namePointer) native.free(namePointer);
        }
    }

    /** The didClose notification. */
    close(uri: string): void {
        const server = this.#open();
        const name = utf8Encoder.encode(uri);
        const namePointer = place(name);
        try {
            pass(native.markdown_core_lsp_close(server, namePointer, name.length));
        } finally {
            native.free(namePointer);
        }
    }

    /**
     * The completion request at `position` in the document at `uri`. It
     * locates the document's session at the cursor and reads the context
     * there.
     *
     * Returns the completions, computed for the document's current version.
     * A host drops completions whose version is older than the document's,
     * as the protocol's `ContentModified` does.
     */
    complete(uri: string, position: Position): Completions {
        const server = this.#open();
        checkPosition(position);
        const name = utf8Encoder.encode(uri);
        let namePointer = 0;
        let cursor = 0;
        let answer = 0;
        try {
            namePointer = place(name);
            cursor = reserve(positionBytes);
            answer = reserve(pointerBytes);
            const view = new DataView(native.memory.buffer);
            view.setUint32(cursor, position.line, true);
            view.setUint32(cursor + 4, position.character, true);
            pass(native.markdown_core_lsp_complete(server, namePointer, name.length, cursor, answer));
            const list = new DataView(native.memory.buffer).getUint32(answer, true);
            try {
                return {
                    version: native.markdown_core_lsp_list_version(list),
                    count: native.markdown_core_lsp_list_count(list) >>> 0
                };
            } finally {
                native.markdown_core_lsp_list_free(list);
            }
        } finally {
            if (answer) native.free(answer);
            if (cursor) native.free(cursor);
            if (namePointer) native.free(namePointer);
        }
    }

    /** Releases the server's WebAssembly state and every document it holds;
     * a second call does nothing, and every later call throws an `Error`. */
    dispose(): void {
        servers.unregister(this);
        if (this.#engine.server) release(this.#engine);
    }

    /** The WebAssembly server, which a disposed server no longer has. */
    #open(): number {
        if (!this.#engine.server) throw new Error("The server is disposed.");
        return this.#engine.server;
    }
}
