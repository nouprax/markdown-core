export type NativeKind =
    | "document"
    | "blockQuote"
    | "paragraph"
    | "heading"
    | "thematicBreak"
    | "list"
    | "listItem"
    | "codeBlock"
    | "htmlBlock"
    | "formulaBlock"
    | "table"
    | "tableRow"
    | "tableCell"
    | "directiveBlock"
    | "directiveLabel"
    | "footnoteDefinition"
    | "text"
    | "softBreak"
    | "lineBreak"
    | "code"
    | "html"
    | "formula"
    | "emphasis"
    | "strong"
    | "strikethrough"
    | "link"
    | "image"
    | "directive"
    | "footnoteReference";

export const kinds: readonly (NativeKind | "none")[] = Object.freeze([
    "none",
    "document",
    "blockQuote",
    "paragraph",
    "heading",
    "thematicBreak",
    "list",
    "listItem",
    "codeBlock",
    "htmlBlock",
    "formulaBlock",
    "table",
    "tableRow",
    "tableCell",
    "directiveBlock",
    "directiveLabel",
    "footnoteDefinition",
    "text",
    "softBreak",
    "lineBreak",
    "code",
    "html",
    "formula",
    "emphasis",
    "strong",
    "strikethrough",
    "link",
    "image",
    "directive",
    "footnoteReference"
]);
