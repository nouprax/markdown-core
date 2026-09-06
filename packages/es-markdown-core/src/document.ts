import type { Document as DocumentValue } from "./model/document.js";
import { parseDocument } from "./runtime/parser.js";

export type Document = DocumentValue;

interface DocumentParser {
    /** Parses `source` as the one Markdown Core dialect. There is nothing to
     * configure: every feature is recognized on every call. */
    parse(source: string): Document;
}

export const Document: DocumentParser = {
    parse: parseDocument
};
