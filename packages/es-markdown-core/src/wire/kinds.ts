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
    | "footnoteReference"
    | "crossLink"
    | "embed"
    | "referenceDefinition"
    | "linkReference"
    | "imageReference";

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
    "footnoteReference",
    "crossLink",
    "embed",
    // Appended: this array is indexed by the native kind value, so a kind
    // inserted in the middle would renumber every kind after it.
    "referenceDefinition",
    "linkReference",
    "imageReference"
]);
