import type { BlockQuote } from "./block-quote.js";
import type { CodeBlock } from "./code-block.js";
import type { Code } from "./code.js";
import type { DirectiveBlock } from "./directive-block.js";
import type { Directive } from "./directive.js";
import type { Document } from "./document.js";
import type { Emphasis } from "./emphasis.js";
import type { FootnoteDefinition, FootnoteReference } from "./footnote.js";
import type { FormulaBlock } from "./formula-block.js";
import type { Formula } from "./formula.js";
import type { Heading } from "./heading.js";
import type { HTMLBlock } from "./html-block.js";
import type { HTML } from "./html.js";
import type { Image } from "./image.js";
import type { LineBreak } from "./line-break.js";
import type { Link } from "./link.js";
import type { List, ListItem } from "./list.js";
import type { Paragraph } from "./paragraph.js";
import type { SoftBreak } from "./soft-break.js";
import type { Strikethrough } from "./strikethrough.js";
import type { Strong } from "./strong.js";
import type { Table, TableCell, TableRow } from "./table.js";
import type { Text } from "./text.js";
import type { ThematicBreak } from "./thematic-break.js";

export type Markup =
    | Document
    | BlockQuote
    | Paragraph
    | Heading
    | ThematicBreak
    | List
    | ListItem
    | CodeBlock
    | HTMLBlock
    | FormulaBlock
    | Table
    | TableRow
    | TableCell
    | DirectiveBlock
    | FootnoteDefinition
    | Text
    | SoftBreak
    | LineBreak
    | Code
    | HTML
    | Formula
    | Emphasis
    | Strong
    | Strikethrough
    | Link
    | Image
    | Directive
    | FootnoteReference;
