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
    | "cite"
    | "tableRow"
    | "tableCell"
    | "directiveLabel"
    | "comment"
    | "crossLink";

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
    "cite",
    "tableRow",
    "tableCell",
    "directiveLabel",
    "comment",
    "crossLink"
]);
