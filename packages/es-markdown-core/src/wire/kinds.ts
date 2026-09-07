export type NativeKind =
    | "document"
    | "callout"
    | "paragraph"
    | "heading"
    | "thematicBreak"
    | "list"
    | "listItem"
    | "codeBlock"
    | "htmlBlock"
    | "formulaBlock"
    | "table"
    | "directiveBlock"
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
    | "footnoteReference"
    | "tableRow"
    | "tableCell"
    | "directiveLabel"
    | "comment";

export const kinds: readonly (NativeKind | "none")[] = Object.freeze([
    "none",
    "document",
    "callout",
    "paragraph",
    "heading",
    "thematicBreak",
    "list",
    "listItem",
    "codeBlock",
    "htmlBlock",
    "formulaBlock",
    "table",
    "directiveBlock",
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
    "footnoteReference",
    "tableRow",
    "tableCell",
    "directiveLabel",
    "comment"
]);
