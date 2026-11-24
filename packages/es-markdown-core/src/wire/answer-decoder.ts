import { MarkdownCoreError } from "../common/markdown-core-error.js";
import type { Scope } from "../markup/values.js";
import type { NativeExports } from "../runtime/native.js";
import { errorCodes, lengthOffset, statusOffset } from "./node-decoder.js";

/*
 * The reader of the MCB3 answers of a session's queries
 * (docs/architecture/wire-format.md, Queries), which share the document's
 * header: status 0 is the answer's body, and 1 a failure, whose body is its
 * u32 `markdown_core_status`.
 */

/** Reads and releases one owned answer: what `read` makes of its body, or
 * its failure thrown as a `MarkdownCoreError`. Zero is an answer that could
 * not be allocated. */
export function readAnswer<T>(nativeExports: NativeExports, message: number, read: (reader: Answer) => T): T {
    if (!message) throw new MarkdownCoreError("allocationFailed");
    try {
        // A call may grow memory, which detaches every pre-call view, so the
        // views are taken here, after the call, and none escapes the try.
        const length = new DataView(nativeExports.memory.buffer).getUint32(message + lengthOffset, true);
        return read(new Answer(new Uint8Array(nativeExports.memory.buffer, message, length)));
    } finally {
        nativeExports.markdown_core_wire_free(message);
    }
}

/** One answer's body, read in order. */
export class Answer {
    private readonly view: DataView;
    private offset = statusOffset;

    constructor(bytes: Uint8Array) {
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        if (this.u8() === 1) throw new MarkdownCoreError(errorCodes[this.u32()]!);
    }

    scopes(): Scope[] {
        return this.list(() => this.scope());
    }

    private scope(): Scope {
        return {
            start: { line: this.i32(), column: this.i32() },
            end: { line: this.i32(), column: this.i32() }
        };
    }

    private list<T>(read: () => T): T[] {
        const count = this.u32();
        const items: T[] = [];
        for (let index = 0; index < count; index += 1) items.push(read());
        return items;
    }

    private u8(): number {
        return this.view.getUint8(this.offset++);
    }

    private u32(): number {
        const value = this.view.getUint32(this.offset, true);
        this.offset += 4;
        return value;
    }

    private i32(): number {
        const value = this.view.getInt32(this.offset, true);
        this.offset += 4;
        return value;
    }
}
