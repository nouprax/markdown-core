import { MarkdownCoreError } from "../common/markdown-core-error.js";
import type { Edit, Opener, Slot } from "../markup/context.js";
import type { Markup } from "../markup/markup.js";
import type { Scope, TextUnit } from "../markup/values.js";
import { native, type NativeExports } from "../runtime/native.js";
import { kinds, slotKinds, type SlotKind } from "./kinds.js";
import { errorCodes, lengthOffset, statusOffset } from "./node-decoder.js";

/*
 * The reader of the MCB3 answers of a session's queries
 * (docs/architecture/wire-format.md, Queries), which share the document's
 * header: status 0 is the answer's body, and 1 a failure, whose body is its
 * u32 `markdown_core_status`.
 */

/** A slot as its answer carries it, before its session lets it spell. */
export type SlotFields = Omit<Slot, "spell">;

/** A context as its answer carries it. */
export interface ContextFields {
    readonly blocks: readonly Opener[];
    readonly inlines: readonly Opener[];
    readonly slots: readonly SlotFields[];
}

/** The names and characters the dialect fixes, which no session holds. */
export interface Lexicon {
    readonly fields: readonly string[];
    readonly cues: readonly string[];
}

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

let lexicon: Lexicon | undefined;

/** The metadata field names and the slot cues, read from the engine once. */
export function dialect(): Lexicon {
    return (lexicon ??= readAnswer(native, native.markdown_core_wire_dialect(), (answer) => answer.dialect()));
}

/** One answer's body, read in order. */
export class Answer {
    private readonly view: DataView;
    private readonly utf8 = new TextDecoder("utf-8", { ignoreBOM: true });
    private offset = statusOffset;

    constructor(private readonly bytes: Uint8Array) {
        this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        if (this.u8() === 1) throw new MarkdownCoreError(errorCodes[this.u32()]!);
    }

    context(): ContextFields {
        const blocks = this.list(() => this.opener());
        const inlines = this.list(() => this.opener());
        const slots = this.list((): SlotFields => ({
            kind: slotKinds[this.u8()] as SlotKind,
            owner: kinds[this.u8()] as Markup["kind"],
            before: this.scope(),
            after: this.scope(),
            closed: this.u8() === 1,
            prefix: this.list(() => this.string())
        }));
        return { blocks, inlines, slots };
    }

    /** The spelling, with its caret counted in `unit`. */
    edit(unit: TextUnit): Edit | null {
        if (this.u8() === 0) return null;
        const length = this.u32();
        const text = this.bytes.subarray(this.offset, (this.offset += length));
        const cursor = this.u32();
        // The caret stands after the value, at a scalar boundary.
        const columns = unit === "utf8" ? cursor : this.utf8.decode(text.subarray(0, cursor)).length;
        return { text: this.utf8.decode(text), cursor: columns };
    }

    scopes(): Scope[] {
        return this.list(() => this.scope());
    }

    dialect(): Lexicon {
        const fields = this.list(() => this.string());
        // Every cue is one ASCII byte.
        return { fields, cues: [...this.string()] };
    }

    private opener(): Opener {
        const kind = this.u8();
        return { kind: kind === 0 ? null : (kinds[kind] as Markup["kind"]), scope: this.scope() };
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

    private string(): string {
        const length = this.u32();
        this.offset += length;
        return this.utf8.decode(this.bytes.subarray(this.offset - length, this.offset));
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
