import MarkdownCoreC

/// Copies the native tree into records once, with no recursion: every native
/// node is queued in breadth-first order, so each node's children follow it,
/// and records are built from the last queued node back to the root. No native
/// pointer survives the copy.
struct DocumentBuilder {
    /// Queue positions of one native node's owned relations.
    private struct Relations {
        var children: [Int] = []
        var metadata: Int?
        var caption: Int?
        var label: Int?
        var title: [Int] = []
        var term: [Int] = []
        var bodies: [[Int]] = []
        var citations: [Int] = []
        var note: Int?
        var prefix: [Int] = []
        var suffix: [Int] = []
    }

    private let document: OpaquePointer
    private let unit: TextUnit
    private var queue: [OpaquePointer] = []
    private var relations: [Relations] = []
    private var records: [MarkupRecord?] = []
    /// The records the document's tables may name: every footnote, specimen
    /// and reference, and every heading a label may resolve to.
    private var definitions: [OpaquePointer: MarkupRecord] = [:]

    /// Queues the tree below `root`, reading each node's children with one
    /// native cursor. Throws ``MarkdownCoreError`` with
    /// ``ErrorCode/allocationFailed`` when the cursor cannot allocate.
    init(document: OpaquePointer, root: OpaquePointer, unit: TextUnit) throws {
        self.document = document
        self.unit = unit
        var native: OpaquePointer?
        let status = markdown_core_cursor_new(root, &native)
        guard status == MARKDOWN_CORE_OK, let cursor = native else { throw MarkdownCoreError(status) }
        defer { markdown_core_cursor_free(cursor) }
        queue = [root]
        var index = 0
        while index < queue.count {
            let owned = try scan(queue[index], with: cursor)
            relations.append(owned)
            index += 1
        }
    }

    mutating func build() -> DocumentRecord {
        records = Array(repeating: nil, count: queue.count)
        for index in queue.indices.reversed() {
            let built = record(from: queue[index], relations: relations[index])
            switch markdown_core_node_get_kind(queue[index]) {
            case MARKDOWN_CORE_KIND_FOOTNOTE, MARKDOWN_CORE_KIND_SPECIMEN, MARKDOWN_CORE_KIND_REFERENCE,
                MARKDOWN_CORE_KIND_HEADING:
                definitions[queue[index]] = built
            default: break
            }
            records[index] = built
        }
        return unsafeDowncast(take(0), to: DocumentRecord.self)
    }

    /// Queues each child the cursor reads of `node`, in canonical traversal
    /// order, in the relation its field names: a Definition's content is its
    /// bodies, the list the cursor names, and every body is kept, an empty one
    /// included. The node's first list field -- content, a table's rows, a
    /// row's cells, a list's definitions -- is its children.
    private mutating func scan(_ node: OpaquePointer, with cursor: OpaquePointer) throws -> Relations {
        var relations = Relations()
        let kind = markdown_core_node_get_kind(node)
        if kind == MARKDOWN_CORE_KIND_DEFINITION {
            let count = answer(0) { markdown_core_node_definition_bodies(node, $0) }
            relations.bodies = Array(repeating: [], count: count)
        }
        markdown_core_cursor_reset(cursor, node)
        var moved = false
        let status = markdown_core_cursor_child(cursor, &moved)
        guard status == MARKDOWN_CORE_OK else { throw MarkdownCoreError(status) }
        while moved {
            let index = enqueue(markdown_core_cursor_node(cursor))
            switch markdown_core_cursor_field(cursor) {
            case MARKDOWN_CORE_FIELD_METADATA: relations.metadata = index
            case MARKDOWN_CORE_FIELD_CAPTION: relations.caption = index
            case MARKDOWN_CORE_FIELD_LABEL: relations.label = index
            case MARKDOWN_CORE_FIELD_TITLE: relations.title.append(index)
            case MARKDOWN_CORE_FIELD_TERM: relations.term.append(index)
            case MARKDOWN_CORE_FIELD_CITATIONS: relations.citations.append(index)
            case MARKDOWN_CORE_FIELD_NOTE: relations.note = index
            case MARKDOWN_CORE_FIELD_PREFIX: relations.prefix.append(index)
            case MARKDOWN_CORE_FIELD_SUFFIX: relations.suffix.append(index)
            case MARKDOWN_CORE_FIELD_CONTENT where kind == MARKDOWN_CORE_KIND_DEFINITION:
                relations.bodies[markdown_core_cursor_list(cursor)].append(index)
            // CONTENT, HEAD, FOOT, CELLS and DEFINITIONS: the first list field.
            default: relations.children.append(index)
            }
            moved = markdown_core_cursor_next(cursor)
        }
        return relations
    }

    private mutating func enqueue(_ value: OpaquePointer) -> Int {
        let index = queue.count
        queue.append(value)
        return index
    }

    /// Moves a built record out of the queue, so its parent alone owns it.
    private mutating func take(_ index: Int) -> MarkupRecord {
        let record = records[index]
        records[index] = nil
        // swift-format-ignore: NeverForceUnwrap
        return record!
    }

    private mutating func take(_ indices: [Int]) -> [MarkupRecord] {
        indices.map { take($0) }
    }

    private mutating func take<Node: MarkupRecord>(_ index: Int?, as _: Node.Type) -> Node? {
        index.map { unsafeDowncast(take($0), to: Node.self) }
    }

    /// The definitions the document's table names, in its order.
    private func table<Node: MarkupRecord>(
        count: Int,
        at entry: (Int) -> OpaquePointer?,
        as _: Node.Type
    ) -> [Node] {
        // Every index is below the count, so each entry is a definition built
        // from the tree.
        // swift-format-ignore: NeverForceUnwrap
        (0..<count).map { unsafeDowncast(definitions[entry($0)!]!, to: Node.self) }
    }

    /// Each label that resolves, keyed by its UTF-8 bytes, with the reference
    /// or heading the document's label table names for it. The engine
    /// normalizes and resolves labels; the binding only copies its answer.
    private func referenceLabels() -> [[UInt8]: MarkupRecord] {
        var labels: [[UInt8]: MarkupRecord] = [:]
        for index in 0..<markdown_core_document_reference_label_count(document) {
            var label = markdown_core_string()
            var node: OpaquePointer?
            answered(markdown_core_document_reference_label_at(document, index, &label, &node))
            // The table names only references and headings of the tree.
            // swift-format-ignore: NeverForceUnwrap
            labels[Array(UnsafeBufferPointer(start: label.data, count: label.length))] = definitions[node!]!
        }
        return labels
    }
}

extension DocumentBuilder {
    // Keep the exhaustive native-kind switch in one place so a newly added native
    // kind cannot silently bypass value-tree copying.
    // swiftlint:disable:next cyclomatic_complexity function_body_length
    private mutating func record(from node: OpaquePointer, relations: Relations) -> MarkupRecord {
        let children = take(relations.children)
        switch markdown_core_node_get_kind(node) {
        case MARKDOWN_CORE_KIND_DOCUMENT:
            let document = document
            return DocumentRecord(
                InheritedFields(from: node),
                unit: unit,
                metadata: take(relations.metadata, as: MetadataRecord.self),
                content: children,
                footnotes: table(
                    count: markdown_core_document_footnote_count(document),
                    at: { index in answer { markdown_core_document_footnote_at(document, index, $0) } },
                    as: FootnoteRecord.self
                ),
                specimens: table(
                    count: markdown_core_document_specimen_count(document),
                    at: { index in answer { markdown_core_document_specimen_at(document, index, $0) } },
                    as: SpecimenRecord.self
                ),
                references: table(
                    count: markdown_core_document_reference_count(document),
                    at: { index in answer { markdown_core_document_reference_at(document, index, $0) } },
                    as: ReferenceRecord.self
                ),
                referenceLabels: referenceLabels()
            )
        case MARKDOWN_CORE_KIND_CITATION:
            return CitationRecord(
                from: node,
                note: take(relations.note, as: FootnoteRecord.self),
                prefix: take(relations.prefix),
                suffix: take(relations.suffix)
            )
        case MARKDOWN_CORE_KIND_FOOTNOTE: return FootnoteRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_SPECIMEN: return SpecimenRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_METADATA: return MetadataRecord(from: node)
        case MARKDOWN_CORE_KIND_REFERENCE: return ReferenceRecord(from: node)
        case MARKDOWN_CORE_KIND_CALLOUT:
            return CalloutRecord(from: node, title: take(relations.title), content: children)
        case MARKDOWN_CORE_KIND_DEFINITION_LIST: return DefinitionListRecord(from: node, definitions: children)
        case MARKDOWN_CORE_KIND_DEFINITION:
            let term = take(relations.term)
            let bodies = relations.bodies.map { body in body.map { take($0) } }
            return DefinitionRecord(from: node, term: term, bodies: bodies)
        case MARKDOWN_CORE_KIND_PARAGRAPH: return ParagraphRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_HEADING: return HeadingRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_THEMATIC_BREAK: return ThematicBreakRecord(from: node)
        case MARKDOWN_CORE_KIND_LIST: return ListRecord(from: node, items: children)
        case MARKDOWN_CORE_KIND_LIST_ITEM: return ListItemRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_CODE_BLOCK: return CodeBlockRecord(from: node)
        case MARKDOWN_CORE_KIND_HTML_BLOCK: return HTMLBlockRecord(from: node)
        case MARKDOWN_CORE_KIND_FORMULA_BLOCK: return FormulaBlockRecord(from: node)
        case MARKDOWN_CORE_KIND_TABLE:
            let caption = take(relations.caption, as: TableCaptionRecord.self)
            return TableRecord(from: node, caption: caption, rows: children)
        case MARKDOWN_CORE_KIND_DIRECTIVE_BLOCK:
            return DirectiveBlockRecord(
                from: node,
                label: take(relations.label, as: DirectiveLabelRecord.self),
                content: children
            )
        case MARKDOWN_CORE_KIND_TEXT: return TextRecord(from: node)
        case MARKDOWN_CORE_KIND_SOFT_BREAK: return SoftBreakRecord(from: node)
        case MARKDOWN_CORE_KIND_LINE_BREAK: return LineBreakRecord(from: node)
        case MARKDOWN_CORE_KIND_CODE: return CodeRecord(from: node)
        case MARKDOWN_CORE_KIND_HTML: return HTMLRecord(from: node)
        case MARKDOWN_CORE_KIND_COMMENT: return CommentRecord(from: node)
        case MARKDOWN_CORE_KIND_CROSS_LINK: return CrossLinkRecord(from: node)
        case MARKDOWN_CORE_KIND_CROSS_EMBEDDED: return CrossEmbeddedRecord(from: node)
        case MARKDOWN_CORE_KIND_FORMULA: return FormulaRecord(from: node)
        case MARKDOWN_CORE_KIND_EMPHASIS: return EmphasisRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_STRONG: return StrongRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_STRIKETHROUGH: return StrikethroughRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_MARK: return MarkRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_INSERTION: return InsertionRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_SPAN: return SpanRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_SUPERSCRIPT: return SuperscriptRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_SUBSCRIPT: return SubscriptRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_LINK: return LinkRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_EMBEDDED: return EmbeddedRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_DIRECTIVE:
            return DirectiveRecord(from: node, label: take(relations.label, as: DirectiveLabelRecord.self))
        case MARKDOWN_CORE_KIND_CITE: return CiteRecord(from: node, citations: take(relations.citations))
        case MARKDOWN_CORE_KIND_TABLE_CAPTION: return TableCaptionRecord(from: node, content: children)
        case MARKDOWN_CORE_KIND_TABLE_ROW: return TableRowRecord(from: node, cells: children)
        case MARKDOWN_CORE_KIND_TABLE_CELL: return TableCellRecord(from: node, content: children)
        // A C enum switch is never exhaustive in Swift; the one kind
        // left is MARKDOWN_CORE_KIND_DIRECTIVE_LABEL.
        default: return DirectiveLabelRecord(from: node, content: children)
        }
    }
}
