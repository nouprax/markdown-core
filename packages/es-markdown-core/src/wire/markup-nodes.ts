import type { Attributes } from "../markup/attributes.js";
import type { Dimensions } from "../common/constraints.js";
import type { Callout } from "../markup/callout.js";
import type { Citation, Cite } from "../markup/cite.js";
import type { CodeBlock } from "../markup/code-block.js";
import type { CrossEmbedded } from "../markup/cross-embedded.js";
import type { CrossLink } from "../markup/cross-link.js";
import type { Definition, DefinitionList } from "../markup/definition-list.js";
import type { DirectiveBlock } from "../markup/directive-block.js";
import type { DirectiveLabel } from "../markup/directive-label.js";
import type { Directive } from "../markup/directive.js";
import type { Document } from "../markup/document.js";
import type { Embedded } from "../markup/embedded.js";
import type { Footnote } from "../markup/footnote.js";
import type { Formula } from "../markup/formula.js";
import type { Heading } from "../markup/heading.js";
import type { Link } from "../markup/link.js";
import type { List, ListItem } from "../markup/list.js";
import type { Markup } from "../markup/markup.js";
import type { Metadata, MetadataValue } from "../markup/metadata.js";
import type { Specimen } from "../markup/specimen.js";
import type { Table, TableCell, TableColumn, TableRow } from "../markup/table.js";
import type {
    CitationReferent,
    Destination,
    ListFlavor,
    OrderedListDelimiter,
    OrderedListVariant,
    Placement,
    Scope
} from "../markup/values.js";
import { MarkupDumper } from "../visitor/markup-dumper.js";

/*
 * One constructor per node shape. Every node of a kind assigns the same
 * fields in the same order, so all of them share one hidden class, and
 * `dump` is a method of the prototype rather than a closure and a property
 * descriptor installed on each instance after construction.
 */

/** The fields every node carries, in the order every node assigns them. */
export class MarkupNode<Kind extends string> {
    constructor(
        readonly kind: Kind,
        readonly scope: Scope,
        readonly anchor: string | null,
        readonly attributes: Attributes
    ) {}

    /** Returns the canonical debug dump for this markup subtree. */
    dump(): string {
        return MarkupDumper.dump(this as unknown as Markup);
    }
}

export type ContainerKind =
    | "paragraph"
    | "emphasis"
    | "strong"
    | "strikethrough"
    | "mark"
    | "insertion"
    | "span"
    | "superscript"
    | "subscript"
    | "directiveLabel"
    | "tableCaption";

export type LiteralKind = "htmlBlock" | "text" | "code" | "html" | "comment" | "formulaBlock";

export type LeafKind = "thematicBreak" | "softBreak" | "lineBreak";

/** A node whose only field is its ordinary content. */
export class ContainerNode<Kind extends ContainerKind> extends MarkupNode<Kind> {
    constructor(
        kind: Kind,
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly content: readonly Markup[]
    ) {
        super(kind, scope, anchor, attributes);
    }
}

/** A node whose only field is its literal. */
export class LiteralNode<Kind extends LiteralKind> extends MarkupNode<Kind> {
    constructor(
        kind: Kind,
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly literal: string
    ) {
        super(kind, scope, anchor, attributes);
    }
}

export class DocumentNode extends MarkupNode<"document"> implements Document {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly content: readonly Markup[],
        readonly metadata: Metadata | null,
        readonly footnotes: readonly Footnote[],
        readonly specimens: readonly Specimen[]
    ) {
        super("document", scope, anchor, attributes);
    }
}

export class CiteNode extends MarkupNode<"cite"> implements Cite {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly citations: readonly Citation[]
    ) {
        super("cite", scope, anchor, attributes);
    }
}

export class HeadingNode extends MarkupNode<"heading"> implements Heading {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly level: number,
        readonly content: readonly Markup[]
    ) {
        super("heading", scope, anchor, attributes);
    }
}

export class CalloutNode extends MarkupNode<"callout"> implements Callout {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly variant: string | null,
        readonly collapsed: boolean | null,
        readonly title: readonly Markup[] | null,
        readonly content: readonly Markup[]
    ) {
        super("callout", scope, anchor, attributes);
    }
}

export class ListNode extends MarkupNode<"list"> implements List {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly flavor: ListFlavor,
        readonly start: number | null,
        readonly variant: OrderedListVariant | null,
        readonly delimiter: OrderedListDelimiter | null,
        readonly tight: boolean,
        readonly items: readonly ListItem[]
    ) {
        super("list", scope, anchor, attributes);
    }
}

export class ListItemNode extends MarkupNode<"listItem"> implements ListItem {
    readonly tasked: boolean;
    readonly completed: boolean;

    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly marker: string | null,
        readonly content: readonly Markup[]
    ) {
        super("listItem", scope, anchor, attributes);
        this.tasked = marker !== null;
        this.completed = marker !== null && marker !== " ";
    }
}

export class CodeBlockNode extends MarkupNode<"codeBlock"> implements CodeBlock {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly info: string | null,
        readonly language: string | null,
        readonly literal: string,
        readonly fenced: boolean,
        readonly closed: boolean
    ) {
        super("codeBlock", scope, anchor, attributes);
    }
}

export class FormulaNode extends MarkupNode<"formula"> implements Formula {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly mode: Placement,
        readonly literal: string
    ) {
        super("formula", scope, anchor, attributes);
    }
}

export class TableNode extends MarkupNode<"table"> implements Table {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly caption: ContainerNode<"tableCaption"> | null,
        readonly columns: readonly TableColumn[],
        readonly head: readonly TableRow[],
        readonly content: readonly TableRow[],
        readonly foot: readonly TableRow[]
    ) {
        super("table", scope, anchor, attributes);
    }
}

export class TableRowNode extends MarkupNode<"tableRow"> implements TableRow {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly cells: readonly TableCell[]
    ) {
        super("tableRow", scope, anchor, attributes);
    }
}

export class TableCellNode extends MarkupNode<"tableCell"> implements TableCell {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly rowspan: number,
        readonly colspan: number,
        readonly content: readonly Markup[]
    ) {
        super("tableCell", scope, anchor, attributes);
    }
}

export class DefinitionListNode extends MarkupNode<"definitionList"> implements DefinitionList {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly definitions: readonly Definition[]
    ) {
        super("definitionList", scope, anchor, attributes);
    }
}

export class DefinitionNode extends MarkupNode<"definition"> implements Definition {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly term: readonly Markup[],
        readonly content: readonly (readonly Markup[])[],
        readonly compact: boolean
    ) {
        super("definition", scope, anchor, attributes);
    }
}

export class DirectiveBlockNode extends MarkupNode<"directiveBlock"> implements DirectiveBlock {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly name: string | null,
        readonly label: DirectiveLabel | null,
        readonly content: readonly Markup[]
    ) {
        super("directiveBlock", scope, anchor, attributes);
    }
}

export class DirectiveNode extends MarkupNode<"directive"> implements Directive {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly name: string,
        readonly label: DirectiveLabel | null
    ) {
        super("directive", scope, anchor, attributes);
    }
}

export class CrossLinkNode extends MarkupNode<"crossLink"> implements CrossLink {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly dest: Destination,
        readonly label: string | null
    ) {
        super("crossLink", scope, anchor, attributes);
    }
}

export class CrossEmbeddedNode extends MarkupNode<"crossEmbedded"> implements CrossEmbedded {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly dest: Destination,
        readonly label: string | null,
        readonly dimensions: Dimensions | null
    ) {
        super("crossEmbedded", scope, anchor, attributes);
    }
}

export class LinkNode extends MarkupNode<"link"> implements Link {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly dest: Destination,
        readonly title: string | null,
        readonly content: readonly Markup[]
    ) {
        super("link", scope, anchor, attributes);
    }
}

export class EmbeddedNode extends MarkupNode<"embedded"> implements Embedded {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly dest: Destination,
        readonly title: string | null,
        readonly dimensions: Dimensions | null,
        readonly content: readonly Markup[]
    ) {
        super("embedded", scope, anchor, attributes);
    }
}

export class CitationNode extends MarkupNode<"citation"> implements Citation {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly referent: CitationReferent,
        readonly prefix: readonly Markup[],
        readonly suffix: readonly Markup[]
    ) {
        super("citation", scope, anchor, attributes);
    }
}

export class FootnoteNode extends MarkupNode<"footnote"> implements Footnote {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly id: string,
        readonly content: readonly Markup[]
    ) {
        super("footnote", scope, anchor, attributes);
    }
}

export class SpecimenNode extends MarkupNode<"specimen"> implements Specimen {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly id: string | null,
        readonly start: number | null,
        readonly content: readonly Markup[]
    ) {
        super("specimen", scope, anchor, attributes);
    }
}

export class MetadataNode extends MarkupNode<"metadata"> implements Metadata {
    constructor(
        scope: Scope,
        anchor: string | null,
        attributes: Attributes,
        readonly name: MetadataValue | null,
        readonly title: MetadataValue | null,
        readonly subtitle: MetadataValue | null,
        readonly time: MetadataValue | null,
        readonly date: MetadataValue | null,
        readonly authors: MetadataValue | null,
        readonly keywords: MetadataValue | null,
        readonly abstract: MetadataValue | null,
        readonly state: MetadataValue | null,
        readonly comment: MetadataValue | null
    ) {
        super("metadata", scope, anchor, attributes);
    }
}
