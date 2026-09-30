import type { Document as DocumentValue } from "./markup/document.js";
import type { TextUnit } from "./markup/values.js";
import { parseDocument } from "./runtime/parser.js";

export type Document = DocumentValue;

interface DocumentParser {
    /** Parses `source` as the one Markdown Core dialect. There is nothing to
     * configure: every feature is recognized on every call. `unit` is the
     * unit every column of the document's scope queries counts in: `"utf16"`,
     * the unit of JavaScript strings and editors, by default. */
    parse(source: string, options?: { readonly unit?: TextUnit }): Document;
}

export const Document: DocumentParser = {
    parse: (source, options) => parseDocument(source, options?.unit ?? "utf16")
};
