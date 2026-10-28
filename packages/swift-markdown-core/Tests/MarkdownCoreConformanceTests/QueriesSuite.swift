import Foundation
import MarkdownCore
import Testing

/// The bindings' query corpus (specs/context): what the C queries answer at
/// the cursor of every example of the context fixture, in both units, which
/// the Swift queries must answer line for line.
@Suite("queries") struct QueriesSuite {
    @Test("the context, spelling, path, field and marker queries answer as the C queries do")
    func corpus() throws {
        let resource = try #require(Bundle.module.url(forResource: "context-queries", withExtension: "json"))
        let corpus = try JSONDecoder().decode(QueriesCorpus.self, from: Data(contentsOf: resource))
        #expect(corpus.schemaVersion == 1)
        #expect(!corpus.cases.isEmpty)
        for testCase in corpus.cases {
            for (unit, expected) in [(TextUnit.utf8, testCase.utf8), (TextUnit.utf16, testCase.utf16)] {
                let position = Position(line: expected.cursor[0], column: expected.cursor[1])
                #expect(
                    try render(testCase.source, unit: unit, at: position, values: corpus.values) == expected.expected,
                    Testing.Comment(rawValue: "context.txt:\(testCase.line) in \(unit)")
                )
            }
        }
    }
}

/// Every query at `position` of `source`, asked of a finalized session of
/// it and rendered as specs/context's README says.
private func render(_ source: String, unit: TextUnit, at position: Position, values: [String]) throws -> String {
    let session = try MarkdownSession(source, unit: unit)
    let document = try session.finalize()
    try session.locate(position)
    let context = session.context
    var lines: [String] = []
    for block in context.blocks {
        lines.append("block \(block.kind.map(String.init(describing:)) ?? "None") \(text(block.scope))")
    }
    for opener in context.inlines {
        lines.append("inline \(opener.kind.map(String.init(describing:)) ?? "None") \(text(opener.scope))")
    }
    for slot in context.slots {
        let prefix = slot.prefix.map { " prefix=\"\($0)\"" }.joined()
        lines.append(
            "slot \(slot.kind) \(slot.owner) \(slot.closed ? "closed" : "open") "
                + "before=\(text(slot.before)) after=\(text(slot.after))\(prefix)"
        )
    }
    for (index, slot) in context.slots.enumerated() {
        let spellings = try session.spell(values, in: slot)
        for (value, spelled) in zip(values, spellings) {
            lines.append("spell \(index) \"\(value)\" " + (spelled.map { "\"\($0.text)\" \($0.cursor)" } ?? "none"))
        }
    }
    let path = try session.path(at: position)
    lines.append((["path"] + path.map { String($0.id.value) }).joined(separator: " "))
    var visitor = IdentifierVisitor()
    document.walk(with: &visitor)
    for node in visitor.nodes {
        var line = "node \(node.id.value)"
        for kind in Slot.Kind.allCases.map(Optional.some) + [nil] {
            let scopes = try session.scope(of: node, field: kind)
            if !scopes.isEmpty { line += " \(kind.map(String.init(describing:)) ?? "none")=\(text(scopes))" }
        }
        let markers = try session.markers(of: node)
        if !markers.isEmpty { line += " markers=\(text(markers))" }
        lines.append(line)
    }
    return lines.map { $0 + "\n" }.joined()
}

private func text(_ scope: Scope) -> String {
    "\(scope.start.line):\(scope.start.column)..\(scope.end.line):\(scope.end.column)"
}

private func text(_ scopes: [Scope]) -> String {
    scopes.map(text).joined(separator: ",")
}

private struct QueriesCorpus: Decodable {
    let schemaVersion: Int
    let values: [String]
    let cases: [QueriesCase]
}

private struct QueriesCase: Decodable {
    let line: Int
    let source: String
    let utf8: QueriesAnswer
    let utf16: QueriesAnswer
}

private struct QueriesAnswer: Decodable {
    let cursor: [Int32]
    let expected: String
}
