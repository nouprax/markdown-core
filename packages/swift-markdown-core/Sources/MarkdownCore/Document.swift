import MarkdownCoreC

/// Why a parse produced no document.
///
/// These are failures of the parse operation itself, not syntax observations.
public enum ParseErrorCode: Int32, Sendable {
    /// The call itself was wrong — a null source, or a length that does not
    /// describe it.
    case invalidArgument = 1
    /// An allocation failed. The parse is abandoned rather than returning a
    /// document with something missing from it.
    case allocationFailed = 2
    /// The parser reached a state it does not otherwise account for.
    case `internal` = 3
}

/// A parse failure, and nothing else.
///
/// It carries no scope: an input the parser could not turn into a document has
/// no document extent to point at.
public struct ParseError: Error, Sendable {
    /// Which failure it was.
    public let code: ParseErrorCode
    /// A fixed English sentence naming the failure. It is for a log, not for
    /// an end user, and it is not localised.
    public let message: String
}

/// The immutable semantic root returned by a parse.
///
/// Every footnote and specimen definition stays in the tree where it was
/// written. The document lists them in source order and looks a label up in
/// those lists; it owns no definition of its own.
public struct Document: Markup {
    let record: DocumentRecord

    /// The node's identifier within its document.
    public var id: MarkupID { record.id }
    /// The whole document's extent. See ``Extent``.
    public var extent: Extent { record.extent }
    /// The explicit anchor, absent when none was attached.
    public var anchor: String? { record.anchor }
    /// Ordered classes and records, including duplicates.
    public var attributes: Attributes { record.attributes }
    /// The document's blocks. Block content, not inline.
    public var content: MarkupCollection<any Markup> {
        record.collection(record.metadataCount..<record.children.count)
    }
    /// The parsed Properties node, absent when no Properties block was authored.
    public var metadata: Metadata? {
        record.metadataCount == 0
            ? nil : Metadata(record: unsafeDowncast(record.children[0], to: MetadataRecord.self))
    }
    /// How the document's scope queries count columns.
    public var unit: TextUnit { record.unit }
    /// Every footnote in source order: each definition where it was written,
    /// and each inline note at its call site.
    public var footnotes: MarkupCollection<Footnote> { MarkupCollection(records: record.footnotes[...]) }
    /// Every specimen definition in source order.
    public var specimens: MarkupCollection<Specimen> { MarkupCollection(records: record.specimens[...]) }

    /// The first footnote in source order whose label equals `label` byte for
    /// byte, or `nil`. An inline note has no label and is never found.
    public func footnote(for label: String) -> Footnote? {
        record.footnoteLabels[Array(label.utf8)].map { Footnote(record: $0) }
    }

    /// The first specimen in source order whose label equals `label` byte for
    /// byte, or `nil`. An anonymous definition is never found.
    public func specimen(for label: String) -> Specimen? {
        record.specimenLabels[Array(label.utf8)].map { Specimen(record: $0) }
    }

    /// Parses `source` and returns the whole tree as values.
    ///
    /// There is one language and nothing to configure: every feature of the
    /// Markdown Core dialect is recognised on every call. The native parse is
    /// released before this returns, so the result borrows nothing and is safe
    /// to hold, copy and send across isolation boundaries.
    ///
    /// - Parameters:
    ///   - source: the Markdown to parse. It is read as UTF-8.
    ///   - unit: how the document's scope queries count columns.
    /// - Returns: the parsed document.
    /// - Throws: ``ParseError`` when there is no document to return at all.
    public static func parse(_ source: String, unit: TextUnit = .utf16) throws -> Document {
        var error: OpaquePointer?
        var text = source
        let document = text.withUTF8 { bytes in
            markdown_core_document_parse_in(bytes.baseAddress, bytes.count, unit.native, &error)
        }
        guard let document else {
            defer { markdown_core_error_free(error) }
            throw ParseError(from: error)
        }
        defer { markdown_core_document_free(document) }

        guard let root = markdown_core_document_root(document),
            markdown_core_node_get_kind(root) == MARKDOWN_CORE_KIND_DOCUMENT
        else {
            throw ParseError(code: .internal, message: "parser returned an invalid document tree")
        }
        var builder = DocumentBuilder(document: document, root: root, unit: unit)
        return Document(record: builder.build())
    }
}

/// Children: the metadata, when authored, then the content. The definition
/// tables name records of the tree; they own nothing the tree does not.
final class DocumentRecord: MarkupRecord, @unchecked Sendable {
    let unit: TextUnit
    let metadataCount: Int
    let footnotes: [MarkupRecord]
    let specimens: [MarkupRecord]
    let footnoteLabels: [[UInt8]: FootnoteRecord]
    let specimenLabels: [[UInt8]: SpecimenRecord]

    init(
        _ fields: InheritedFields,
        unit: TextUnit,
        metadata: MetadataRecord?,
        content: [MarkupRecord],
        footnotes: [FootnoteRecord],
        specimens: [SpecimenRecord]
    ) {
        let metadatas: [MarkupRecord] = metadata.map { [$0] } ?? []
        self.unit = unit
        metadataCount = metadatas.count
        self.footnotes = footnotes
        self.specimens = specimens
        footnoteLabels = DocumentRecord.labels(of: footnotes, \.label)
        specimenLabels = DocumentRecord.labels(of: specimens, \.label)
        super.init(fields, children: metadatas + content)
    }

    /// Each label's first definition in source order, keyed by its UTF-8 bytes
    /// so that no Unicode equivalence decides a match.
    private static func labels<Entry>(
        of definitions: [Entry],
        _ label: KeyPath<Entry, String?>
    ) -> [[UInt8]: Entry] {
        var labels: [[UInt8]: Entry] = [:]
        for definition in definitions {
            guard let text = definition[keyPath: label] else { continue }
            let key = Array(text.utf8)
            if labels[key] == nil { labels[key] = definition }
        }
        return labels
    }

    override var markup: any Markup { Document(record: self) }

    /// The unit is how a query counts, not a fact of the tree, so it is not
    /// compared.
    override func hasEqualFields(_ other: MarkupRecord) -> Bool {
        metadataCount == unsafeDowncast(other, to: DocumentRecord.self).metadataCount
    }

    override func relation(at step: Int) -> Relation? {
        switch step {
        case 0: Relation(name: nil, indices: 0..<metadataCount)
        case 1: Relation(name: nil, indices: metadataCount..<children.count)
        default: nil
        }
    }
}

extension Document: RecordBacked {
    var base: MarkupRecord { record }
}
