import type { Document as DocumentValue } from "./model/document.js";
import type { ParseOptions } from "./parse-options.js";
import { parse } from "./runtime/parser.js";

export type Document = DocumentValue;

interface DocumentParser {
    parse(source: string, options?: ParseOptions): Document;
}

export const Document: DocumentParser = {
    parse
};
