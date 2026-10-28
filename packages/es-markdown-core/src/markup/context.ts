import type { Markup } from "./markup.js";
import type { Scope } from "./values.js";
import type { SlotKind } from "../wire/kinds.js";
import { dialect } from "../wire/answer-decoder.js";

/**
 * The syntactic context at a cursor: the open state of a parse of the text
 * before it, confirmed against the document parsed from the whole text.
 *
 * A context lists the blocks the text before the cursor leaves open, the
 * inline constructs it leaves open in the inline content that holds the
 * cursor, and the fields an open construct reads at the cursor, which the
 * whole document confirms.
 *
 * A session keeps its context as its state at its cursor:
 * `MarkdownSession.locate` moves the cursor and updates
 * `MarkdownSession.context`, and `MarkdownSession.spell` spells from that
 * state as it stands.
 */
export interface Context {
    /** The blocks the text before the cursor leaves open, outermost first,
     * each with the scope of its opening line up to the cursor. */
    readonly blocks: readonly Opener[];
    /** The constructs the text before the cursor leaves open in the inline
     * content that holds it, earliest first, each with the scope of its
     * opening syntax. */
    readonly inlines: readonly Opener[];
    /** The fields the cursor may be writing. There is more than one only
     * where the grammar itself leaves the text before the cursor ambiguous. */
    readonly slots: readonly Slot[];
}

/** A construct a context finds open. */
export interface Opener {
    /** The kind it opens, or null while that is still open too: a `[` may
     * open a link, a span or a cite. */
    readonly kind: Markup["kind"] | null;
    /** Its opening syntax, or for a block its opening line up to the cursor. */
    readonly scope: Scope;
}

/**
 * A field of kind `kind` of a construct of kind `owner` that the cursor may
 * be writing.
 *
 * A slot names a field of its session's `context` as the locate that read it
 * left it: it spells only while that context stands. How it spells depends on
 * the syntax of the text it was read from, such as the `<...>` around a
 * destination, which its fields do not show.
 */
export interface Slot {
    /** The field's role. */
    readonly kind: SlotKind;
    /** The construct the field belongs to. */
    readonly owner: Markup["kind"];
    /** The field's source from its start to the cursor: what the user has
     * written of it. It does not leave the cursor's line. */
    readonly before: Scope;
    /** The field's continuation from the cursor, as the construct's own
     * grammar reads it. It does not leave the cursor's line. */
    readonly after: Scope;
    /** Whether the whole document finishes the construct after the cursor;
     * otherwise it reads the construct's opener as text. */
    readonly closed: boolean;
    /** The earlier fields the slot depends on, as written, such as the note
     * before a cross link's anchor. */
    readonly prefix: readonly string[];
}

/** What replaces a slot's `before` and `after` to write a value in it:
 * Swift's and Kotlin's `Edit`, which ES names apart from the
 * `Insertion` node. */
export interface Edit {
    /** The value in the field's own syntax, and the shortest closer that
     * finishes the construct when it is not `closed`. */
    readonly text: string;
    /** Where the caret lands: the columns of `text` before it, in the
     * session's unit. It stands after the value, before any closer. */
    readonly cursor: number;
}

export const Slot: {
    /**
     * The characters after which a slot may begin with nothing written yet,
     * in ascending order: the last character of a slot's opener, such as the
     * second `[` of `[[` or the `(` of `](`; a separator before a field, such
     * as the space before an attribute record; and a line ending, before a
     * properties member's name. An editor that asks for completions when one
     * is typed asks wherever a slot opens empty.
     */
    readonly cues: readonly string[];
} = {
    get cues() {
        return dialect().cues;
    }
};
