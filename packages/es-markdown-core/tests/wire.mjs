// MCB3 test support (docs/architecture/wire-format.md): the message the
// native side sends for a source, and a writer for messages the parser never
// produces, so decoder tests state records instead of patching byte offsets.
import assert from "node:assert/strict";
import { native } from "../dist/runtime/native.js";
import { kinds } from "../dist/wire/kinds.js";

const encoder = new globalThis.TextEncoder();

/** A copy of the message the native side produces for `source`. */
export function nativeMessage(source) {
    const encoded = encoder.encode(source);
    const sourcePointer = native.malloc(Math.max(encoded.length, 1));
    assert.notEqual(sourcePointer, 0);
    let message = 0;
    try {
        new Uint8Array(native.memory.buffer, sourcePointer, encoded.length).set(encoded);
        message = native.markdown_core_wire_parse(sourcePointer, encoded.length);
        assert.notEqual(message, 0);
        const length = new DataView(native.memory.buffer).getUint32(message + 4, true);
        return Uint8Array.from(new Uint8Array(native.memory.buffer, message, length));
    } finally {
        if (message) native.markdown_core_wire_free(message);
        native.free(sourcePointer);
    }
}

/** Writes a message body; `document()` and `error()` wrap it in the header. */
export class MessageWriter {
    #bytes = [];
    #next = 1;

    u8(value) {
        this.#bytes.push(value & 0xff);
        return this;
    }

    u32(value) {
        for (let shift = 0; shift < 32; shift += 8) this.u8(value >>> shift);
        return this;
    }

    i32(value) {
        return this.u32(value >>> 0);
    }

    int(value) {
        const bits = BigInt.asUintN(64, BigInt(value));
        for (let shift = 0n; shift < 64n; shift += 8n) this.u8(Number((bits >> shift) & 0xffn));
        return this;
    }

    double(value) {
        const bytes = new Uint8Array(8);
        new DataView(bytes.buffer).setFloat64(0, value, true);
        this.#bytes.push(...bytes);
        return this;
    }

    bool(value) {
        return this.u8(value ? 1 : 0);
    }

    string(value) {
        const bytes = encoder.encode(value);
        this.u32(bytes.length);
        this.#bytes.push(...bytes);
        return this;
    }

    /** `T?`: a presence, then `write(value)` when present. */
    optional(value, write) {
        this.bool(value !== null);
        if (value !== null) write.call(this, value);
        return this;
    }

    attributes({ classes = [], records = [] } = {}) {
        this.u32(classes.length);
        for (const value of classes) this.string(value);
        this.u32(records.length);
        for (const { name, value } of records) this.string(name).string(value);
        return this;
    }

    /** A u64 node id. */
    id(value) {
        return this.int(value);
    }

    /**
     * A record's kind and inherited fields; its own fields follow. Records
     * take the writer's next id unless one is given; an extent and a piece
     * are `[lead, span]`, and a run `[lead, span, length]`.
     */
    record(kind, { id = this.#next, extent = [0, 0], pieces = [], runs = [], anchor = null, attributes } = {}) {
        const ordinal = typeof kind === "number" ? kind : kinds.indexOf(kind);
        assert.ok(ordinal >= 0, `unknown kind ${kind}`);
        this.#next = typeof id === "bigint" ? this.#next : Math.max(this.#next, id + 1);
        this.u8(ordinal).id(id).i32(extent[0]).u32(extent[1]);
        this.u32(pieces.length);
        for (const [lead, span] of pieces) this.i32(lead).u32(span);
        this.u32(runs.length);
        for (const [lead, span, length] of runs) this.i32(lead).u32(span).u32(length);
        this.optional(anchor, this.string);
        return this.attributes(attributes);
    }

    text(literal, options) {
        return this.record("text", options).string(literal);
    }

    /** A document record over the `content` nodes written before it, then
     * its definition tables, which name footnote, specimen and Reference ids,
     * and its reference label table, `[label, id]` pairs in byte order. */
    root(content, { metadata = false, footnotes = [], specimens = [], references = [], labels = [], id, extent } = {}) {
        this.record("document", { id, extent }).u32(content).bool(metadata);
        return this.table(footnotes).table(specimens).table(references).labels(labels);
    }

    /** A definition table: a count, then that many ids. */
    table(ids) {
        this.u32(ids.length);
        for (const id of ids) this.id(id);
        return this;
    }

    /** The reference label table: a count, then each label and its target's id. */
    labels(entries) {
        this.u32(entries.length);
        for (const [label, id] of entries) this.string(label).id(id);
        return this;
    }

    document() {
        return this.#message(0);
    }

    /** A failure message: its `markdown_core_status` and nothing else. */
    error(status) {
        return this.u32(status).#message(1);
    }

    #message(status) {
        const header = [0x4d, 0x43, 0x42, 0x33, 0, 0, 0, 0, status];
        const bytes = Uint8Array.from([...header, ...this.#bytes]);
        new DataView(bytes.buffer).setUint32(4, bytes.length, true);
        return bytes;
    }
}
