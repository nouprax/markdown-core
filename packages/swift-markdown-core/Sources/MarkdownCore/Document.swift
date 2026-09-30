import MarkdownCoreC

/// Why a call of the library failed.
public enum ErrorCode: Sendable, Hashable {
    /// An allocation failed, or the source or a session's edited text exceeds
    /// the engine's 1 GiB capacity. The parse is abandoned rather than
    /// returning a document with something missing from it.
    case allocationFailed
    /// The source is too short for the node a scope or dump reads, a
    /// position's line or column is below 1, or a session rejects an edit's
    /// range: its start after its end, its end past the text, two edits that
    /// overlap, or, in UTF-16, an offset between the two units of one scalar.
    case outOfBounds
    /// A value was read as another kind. It is the engine's code, shared by
    /// every binding; typed Swift nodes never reach it.
    case kindMismatch
}

/// The library's one error: a call that cannot answer without crashing or
/// reading memory it does not own throws this with its ``code``, and nothing
/// else.
public struct MarkdownCoreError: Error, Sendable, Hashable {
    /// Which failure it was.
    public let code: ErrorCode
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
    /// - Throws: ``MarkdownCoreError`` with ``ErrorCode/allocationFailed``
    ///   when an allocation fails or `source` exceeds 1 GiB of UTF-8.
    public static func parse(_ source: String, unit: TextUnit = .utf16) throws -> Document {
        var document: OpaquePointer?
        var text = source
        let status = text.withUTF8 { bytes in
            markdown_core_document_parse_in(bytes.baseAddress, bytes.count, unit.native, &document)
        }
        guard status == MARKDOWN_CORE_OK, let document else { throw MarkdownCoreError(status) }
        defer { markdown_core_document_free(document) }

        return Document(native: document, unit: unit)
    }
}

extension Document {
    /// The value copy of a native document, from a parse or a session. It
    /// borrows nothing from `native`, which may be released right after.
    init(native: OpaquePointer, unit: TextUnit) {
        var builder = DocumentBuilder(document: native, root: markdown_core_document_root(native), unit: unit)
        self.init(record: builder.build())
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
