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
    | "media"
    | "directive"
    | "cite"
    | "tableRow"
    | "tableCell"
    | "directiveLabel"
    | "comment"
    | "crossLink"
    | "mark"
    | "crossEmbedded"
    | "insertion"
    | "span"
    | "superscript"
    | "subscript"
    | "definitionList"
    | "definition"
    | "tableCaption";

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
    "media",
    "directive",
    "cite",
    "tableRow",
    "tableCell",
    "directiveLabel",
    "comment",
    "crossLink",
    "mark",
    "crossEmbedded",
    "insertion",
    "span",
    "superscript",
    "subscript",
    "definitionList",
    "definition",
    "tableCaption"
]);
