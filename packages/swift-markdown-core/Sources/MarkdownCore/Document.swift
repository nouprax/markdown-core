import Foundation
import MarkdownCoreC

/// The feature switches for one document and every document its edits
/// produce, fixed when the first one is parsed.
///
/// Every option defaults to `true`.
public struct ParseOptions: Sendable, Hashable {
    /// Replaces straight quotes, dashes, and ellipses with typographic forms.
    public let smartPunctuation: Bool
    /// Parses footnote definitions and references.
    public let footnotes: Bool
    /// Parses pipe tables.
    public let tables: Bool
    /// Parses `~struck~` and `~~struck~~`; the closer must match the
    /// opener's tilde count.
    public let strikethrough: Bool
    /// Recognizes bare URLs and email addresses as links.
    ///
    /// `<https://x>` is CommonMark's own autolink and is not gated here.
    public let autolinks: Bool
    /// Parses `[ ]`/`[x]` task-list item markers.
    public let taskLists: Bool
    /// Parses formula spans and blocks, including dollar and LaTeX delimiters
    /// and `formula` fenced blocks.
    public let formulas: Bool
    /// Parses the directive grammar: `:name` inline, `::name` leaf and
    /// `:::name` container at the head of a line, each with its `[label]`
    /// and `{attributes}`.
    public let directives: Bool
    /// Parses cross-links written as `[[reference]]`.
    public let crossLinks: Bool
    /// Parses embeds written as `![[reference]]`.
    public let embeds: Bool

    /// Creates a fixed option set; every switch defaults to `true`.
    public init(
        smartPunctuation: Bool = true,
        footnotes: Bool = true,
        tables: Bool = true,
        strikethrough: Bool = true,
        autolinks: Bool = true,
        taskLists: Bool = true,
        formulas: Bool = true,
        directives: Bool = true,
        crossLinks: Bool = true,
        embeds: Bool = true
    ) {
        self.smartPunctuation = smartPunctuation
        self.footnotes = footnotes
        self.tables = tables
        self.strikethrough = strikethrough
        self.autolinks = autolinks
        self.taskLists = taskLists
        self.formulas = formulas
        self.directives = directives
        self.crossLinks = crossLinks
        self.embeds = embeds
    }
}

/// The category of a native parse or edit failure.
public enum ParseErrorCode: Int32, Sendable {
    case invalidArgument = 1
    case allocationFailed = 2
    case `internal` = 3
}

/// A native parse or edit failure: a category and the engine's message.
///
/// No scope. These are the parse failing to RUN, never a verdict on the
/// Markdown, so none of them is attributable to an extent of the input. The
/// failure that is — a directive's `{…}` that did not parse — is a
/// ``Diagnostic``, whose scope is not optional.
public struct ParseError: Error, Sendable, CustomStringConvertible {
    /// The failure category.
    public let code: ParseErrorCode
    /// The engine's actionable description of the failure.
    public let message: String

    /// The engine's message, so string interpolation prints it directly.
    public var description: String { message }
}

extension ParseError: LocalizedError {
    /// The native parser's actionable message — the same text as
    /// ``description`` — so Foundation error presentation (alerts, logs,
    /// `NSError` bridging) never degrades to a bare domain and code.
    public var errorDescription: String? { message }
}

/// One parsed Markdown document: the root of the canonical value tree, the
/// owner of the native parse it came from, and the only entry point.
///
/// ```swift
/// let document = try Document("# Title")
/// let renamed = try document.edit("# Renamed")
/// ```
///
/// There is no session type. A document is created from text and options;
/// editing hands it new text and returns the next document. Options are
/// fixed for a document's whole series — changing what the parser means is a
/// new ``Document``, not an edit.
///
/// The node values are ordinary Swift values with no reference back here:
/// hold them, copy them, put them in a view model. What this class owns is
/// the native parse, which ``edit(_:)`` needs in order to keep identities
/// stable across revisions.
public final class Document: Markup, @unchecked Sendable {
    /// The node's series-scoped identity; see ``MarkupID``.
    public let id: MarkupID
    /// The document revision at which this document's content last changed.
    public let revision: UInt64
    /// The document's absolute source extent: the whole text.
    public let scope: Scope
    /// The document's top-level blocks in source order.
    public let content: [any Markup]
    /// Everything an editor should underline, in source order.
    ///
    /// Empty for almost every document; see ``DiagnosticCode``.
    public let diagnostics: [Diagnostic]
    /// The options this document and its whole series were parsed under.
    public let options: ParseOptions

    /// Nodes from different parses never share an identity even when their
    /// raw values collide.
    public var series: UInt64 { id.series }

    let handle: OpaquePointer
    /// Every node below the root by identity, so an answer addressed by
    /// ``MarkupID`` can hand back the node value rather than the bare id.
    private let index: [UInt64: any Markup]
    /// Parses `markdown` and returns a self-contained document; throws
    /// ``ParseError`` when the engine rejects the input or cannot allocate.
    public init(_ markdown: String, options: ParseOptions = .init()) throws {
        var nativeOptions = options.native
        var nativeError: OpaquePointer?
        var source = markdown
        let handle = try source.withUTF8 { buffer -> OpaquePointer in
            let text = markdown_core_string(data: buffer.baseAddress, length: buffer.count)
            guard let handle = markdown_core_document_new(text, &nativeOptions, &nativeError) else {
                defer { markdown_core_error_free(nativeError) }
                throw ParseError(from: nativeError)
            }
            return handle
        }
        self.handle = handle
        self.options = options
        let series = markdown_core_document_series(handle)
        guard let root = markdown_core_document_root(handle) else {
            markdown_core_document_free(handle)
            throw ParseError(code: .internal, message: "parse produced no document root")
        }
        id = MarkupID(series: series, rawValue: markdown_core_node_get_id(root))
        revision = markdown_core_node_get_revision(root)
        scope = Scope(from: markdown_core_node_scope(root))
        content = Document.project(root, series: series)
        diagnostics = Document.diagnostics(handle)
        index = Document.index(of: content)
    }

    private init(adopting handle: OpaquePointer, options: ParseOptions) throws {
        self.handle = handle
        self.options = options
        let series = markdown_core_document_series(handle)
        guard let root = markdown_core_document_root(handle) else {
            markdown_core_document_free(handle)
            throw ParseError(code: .internal, message: "edit produced no document root")
        }
        id = MarkupID(series: series, rawValue: markdown_core_node_get_id(root))
        revision = markdown_core_node_get_revision(root)
        scope = Scope(from: markdown_core_node_scope(root))
        content = Document.project(root, series: series)
        diagnostics = Document.diagnostics(handle)
        index = Document.index(of: content)
    }

    deinit {
        markdown_core_document_free(handle)
    }

    /// Hands this document new text and returns the document that text
    /// describes.
    ///
    /// Reads the receiver and takes nothing from it: this document stays
    /// usable and may be edited again. Editing it twice gives two lines of
    /// descent, told apart by their revisions — and, like nodes from two
    /// separate parses, nodes from two lines are not comparable.
    ///
    /// There is nothing to synchronize. Every stored property is a `let`,
    /// and the native parse a document owns is only ever READ — including
    /// here, where the successor is built a parse of its own — so concurrent
    /// callers cannot race, which is what `@unchecked Sendable` rests on.
    public func edit(_ markdown: String) throws -> Document {
        var nativeError: OpaquePointer?
        var source = markdown
        let next = source.withUTF8 { buffer -> OpaquePointer? in
            let text = markdown_core_string(data: buffer.baseAddress, length: buffer.count)
            return markdown_core_document_edit(handle, text, &nativeError)
        }
        guard let next else {
            defer { markdown_core_error_free(nativeError) }
            throw ParseError(from: nativeError)
        }
        // Exactly one owner: the adopting initializer frees the native
        // document itself on its only throwing path, so a failure here
        // cannot leak it.
        return try Document(adopting: next, options: options)
    }

    /// This document's node for `id`, or nil when no node has that identity
    /// here.
    ///
    /// Passing an identity from another parse is nil, not a trap: a caller
    /// holding an id from a superseded revision is asking exactly this
    /// question.
    public func node(_ id: MarkupID) -> (any Markup)? {
        guard id.series == self.id.series else { return nil }
        // The root answers for itself. It is a `Markup` like any other and
        // its revision moves whenever the top-level block list changes, so a
        // consumer reconciling by id reaches this call with the document's
        // own id — and the index holds the descendants, not the root.
        return id.rawValue == self.id.rawValue ? self : index[id.rawValue]
    }

    /// Dispatches this node to `visitor`'s matching `visit` overload.
    public func accept<V: MarkupVisitor>(_ visitor: inout V) -> V.Result { visitor.visit(self) }

    /// Two documents are equal exactly when they share ``id`` and ``revision``,
    /// the same rule every other ``Markup`` node follows.
    public static func == (lhs: Document, rhs: Document) -> Bool {
        lhs.id == rhs.id && lhs.revision == rhs.revision
    }

    /// Hashes the identity/revision pair that also defines equality.
    public func hash(into hasher: inout Hasher) {
        hasher.combine(id)
        hasher.combine(revision)
    }

    /// Builds every value below `root`.
    ///
    /// Postorder over an explicit stack, because nesting depth is
    /// input-controlled: a document that PARSED must also project, and the
    /// call stack is not a budget this package may spend on the caller's
    /// behalf. Child arrays assemble in sibling frames, so every node is
    /// built exactly once, from children that are already built.
    private static func project(_ root: OpaquePointer, series: UInt64) -> [any Markup] {
        var frames: [[any Markup]] = [[]]
        var completed: [any Markup] = []
        let builder = MarkupBuilder(series: series) { _ in completed }
        var stack: [(node: OpaquePointer, ready: Bool)] = [(root, false)]
        while let (node, ready) = stack.popLast() {
            if ready {
                completed = frames.removeLast()
                if node == root {
                    return completed
                }
                frames[frames.count - 1].append(builder.markup(from: node))
                continue
            }
            stack.append((node, true))
            frames.append([])
            // Reversed so pops build children in source order.
            var children: [OpaquePointer] = []
            var child = markdown_core_node_get_first_child(node)
            while let current = child {
                children.append(current)
                child = markdown_core_node_get_next_sibling(current)
            }
            for current in children.reversed() {
                stack.append((current, false))
            }
        }
        preconditionFailure("the build never reached the document root")
    }

    private static func index(of content: [any Markup]) -> [UInt64: any Markup] {
        var table: [UInt64: any Markup] = [:]
        // The walker's own child visitor, which is the one place that knows
        // which of a kind's edges are children — reusing it is what keeps a
        // kind from being indexed here and walked there.
        var children = ChildrenVisitor()
        var stack = content
        while let node = stack.popLast() {
            table[node.id.rawValue] = node
            stack.append(contentsOf: node.accept(&children))
        }
        return table
    }

    private static func diagnostics(_ handle: OpaquePointer) -> [Diagnostic] {
        var rows: UnsafePointer<markdown_core_diagnostic>?
        let count = markdown_core_document_diagnostics(handle, &rows)
        guard let rows, count > 0 else { return [] }
        return (0..<count).compactMap { index in
            guard let code = DiagnosticCode(rawValue: Int32(rows[index].code.rawValue)) else { return nil }
            return Diagnostic(code: code, scope: Scope(from: rows[index].scope))
        }
    }
}

extension ParseOptions {
    var native: markdown_core_parse_options {
        markdown_core_parse_options(
            smart_punctuation: smartPunctuation,
            footnotes: footnotes,
            tables: tables,
            strikethrough: strikethrough,
            autolinks: autolinks,
            task_lists: taskLists,
            formulas: formulas,
            directives: directives,
            cross_links: crossLinks,
            embeds: embeds
        )
    }
}

/// Builds one platform value from one native node.
///
/// `children` hands back the values already built for that node's children,
/// which is what lets the build run bottom-up over an explicit stack instead
/// of down the call stack.
struct MarkupBuilder {
    let series: UInt64
    let children: (OpaquePointer) -> [any Markup]

    /// The three things every kind carries, read once per node.
    func track(of node: OpaquePointer) -> MarkupTrack {
        MarkupTrack(
            id: MarkupID(series: series, rawValue: markdown_core_node_get_id(node)),
            revision: markdown_core_node_get_revision(node),
            scope: Scope(from: markdown_core_node_scope(node))
        )
    }

    // Keep the exhaustive native-kind switch in one place so a newly added
    // native kind cannot silently bypass value-tree copying.
    // swiftlint:disable:next cyclomatic_complexity
    func markup(from node: OpaquePointer) -> any Markup {
        switch markdown_core_node_get_kind(node) {
        // A document is only ever the root, and the root is built by
        // Document's own initializer — it owns the native parse, which a
        // child value has no business holding.
        case MARKDOWN_CORE_KIND_DOCUMENT: preconditionFailure("a document cannot be a child node")
        case MARKDOWN_CORE_KIND_BLOCK_QUOTE: BlockQuote(from: node, builder: self)
        case MARKDOWN_CORE_KIND_PARAGRAPH: Paragraph(from: node, builder: self)
        case MARKDOWN_CORE_KIND_HEADING: Heading(from: node, builder: self)
        case MARKDOWN_CORE_KIND_THEMATIC_BREAK: ThematicBreak(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LIST: List(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LIST_ITEM: ListItem(from: node, builder: self)
        case MARKDOWN_CORE_KIND_CODE_BLOCK: CodeBlock(from: node, builder: self)
        case MARKDOWN_CORE_KIND_HTML_BLOCK: HTMLBlock(from: node, builder: self)
        case MARKDOWN_CORE_KIND_FORMULA_BLOCK: FormulaBlock(from: node, builder: self)
        case MARKDOWN_CORE_KIND_TABLE: Table(from: node, builder: self)
        case MARKDOWN_CORE_KIND_TABLE_ROW: TableRow(from: node, builder: self)
        case MARKDOWN_CORE_KIND_TABLE_CELL: TableCell(from: node, builder: self)
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK: DirectiveBlock(from: node, builder: self)
        case MARKDOWN_CORE_KIND_DIRECTIVE_LABEL: DirectiveLabel(from: node, builder: self)
        case MARKDOWN_CORE_KIND_FOOTNOTE_DEFINITION: FootnoteDefinition(from: node, builder: self)
        case MARKDOWN_CORE_KIND_TEXT: Text(from: node, builder: self)
        case MARKDOWN_CORE_KIND_SOFT_BREAK: SoftBreak(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LINE_BREAK: LineBreak(from: node, builder: self)
        case MARKDOWN_CORE_KIND_CODE: Code(from: node, builder: self)
        case MARKDOWN_CORE_KIND_HTML: HTML(from: node, builder: self)
        case MARKDOWN_CORE_KIND_FORMULA: Formula(from: node, builder: self)
        case MARKDOWN_CORE_KIND_EMPHASIS: Emphasis(from: node, builder: self)
        case MARKDOWN_CORE_KIND_STRONG: Strong(from: node, builder: self)
        case MARKDOWN_CORE_KIND_STRIKETHROUGH: Strikethrough(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LINK: Link(from: node, builder: self)
        case MARKDOWN_CORE_KIND_IMAGE: Image(from: node, builder: self)
        case MARKDOWN_CORE_KIND_DIRECTIVE: Directive(from: node, builder: self)
        case MARKDOWN_CORE_KIND_FOOTNOTE_REFERENCE: FootnoteReference(from: node, builder: self)
        case MARKDOWN_CORE_KIND_REFERENCE_DEFINITION: ReferenceDefinition(from: node, builder: self)
        case MARKDOWN_CORE_KIND_LINK_REFERENCE: LinkReference(from: node, builder: self)
        case MARKDOWN_CORE_KIND_IMAGE_REFERENCE: ImageReference(from: node, builder: self)
        case MARKDOWN_CORE_KIND_CROSS_LINK: CrossLink(from: node, builder: self)
        case MARKDOWN_CORE_KIND_EMBED: Embed(from: node, builder: self)
        default: preconditionFailure("native parser returned an unknown node kind")
        }
    }
}
