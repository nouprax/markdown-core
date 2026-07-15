import Foundation

@main
enum CanonicalASTResourceGenerator {
    static func main() {
        do {
            try generate(arguments: Array(CommandLine.arguments.dropFirst()))
        } catch {
            FileHandle.standardError.write(Data("\(error)\n".utf8))
            exit(1)
        }
    }

    private static func generate(arguments: [String]) throws {
        guard arguments.count == 2 else { throw GeneratorFailure.invalidArguments }
        let specDirectory =
            URL(fileURLWithPath: arguments[0], isDirectory: true)
            .resolvingSymlinksInPath()
            .standardizedFileURL
        let output = URL(fileURLWithPath: arguments[1], isDirectory: false)
        let manifestData = try Data(
            contentsOf: specDirectory.appendingPathComponent("manifest.json", isDirectory: false)
        )
        let manifest: CanonicalManifest
        do {
            manifest = try JSONDecoder().decode(CanonicalManifest.self, from: manifestData)
        } catch let error as GeneratorFailure {
            throw error
        } catch {
            throw GeneratorFailure.invalidManifest(String(describing: error))
        }
        guard manifest.schemaVersion == 1, !manifest.cases.isEmpty else {
            throw GeneratorFailure.invalidManifest("schemaVersion 1 requires at least one case")
        }
        guard Set(manifest.cases.map(\.name)).count == manifest.cases.count else {
            throw GeneratorFailure.invalidManifest("case names must be unique")
        }

        let cases = try manifest.cases.map { testCase in
            GeneratedCase(
                name: testCase.name,
                source: try fixtureContents(at: testCase.input, in: specDirectory),
                expected: try fixtureContents(at: testCase.expected, in: specDirectory),
                parseOptions: testCase.parseOptions
            )
        }
        let generated = GeneratedManifest(schemaVersion: manifest.schemaVersion, cases: cases)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(generated)
        try FileManager.default.createDirectory(
            at: output.deletingLastPathComponent(),
            withIntermediateDirectories: true
        )
        try data.write(to: output, options: .atomic)
    }

    private static func fixtureContents(at relativePath: String, in root: URL) throws -> String {
        guard !relativePath.hasPrefix("/") else {
            throw GeneratorFailure.invalidRelativePath(relativePath)
        }
        let rootPrefix = root.path.hasSuffix("/") ? root.path : root.path + "/"
        let url = root.appendingPathComponent(relativePath).resolvingSymlinksInPath()
            .standardizedFileURL
        guard url.path.hasPrefix(rootPrefix) else {
            throw GeneratorFailure.invalidRelativePath(relativePath)
        }
        return try String(contentsOf: url, encoding: .utf8)
    }
}

private enum GeneratorFailure: Error, CustomStringConvertible {
    case invalidArguments
    case invalidManifest(String)
    case invalidRelativePath(String)

    var description: String {
        switch self {
        case .invalidArguments:
            "usage: CanonicalASTResourceGenerator SPEC_DIRECTORY OUTPUT_FILE"
        case .invalidManifest(let reason):
            "invalid canonical AST manifest: \(reason)"
        case .invalidRelativePath(let path):
            "canonical AST case path escapes the spec directory: \(path)"
        }
    }
}

private struct CanonicalManifest: Decodable {
    let schemaVersion: Int
    let contract: String
    let dumpGrammar: String
    let format: CanonicalFormat
    let coverageRequirements: CanonicalCoverage
    let cases: [CanonicalCase]
}

private struct CanonicalFormat: Decodable {
    let encoding: String
    let lineEndings: String
    let finalNewline: Bool
    let caseOrder: String
}

private struct CanonicalCase: Decodable {
    let name: String
    let input: String
    let expected: String
    let parseOptions: CanonicalParseOptions
    let coverage: CanonicalCoverage
}

private struct CanonicalCoverage: Decodable {
    let kinds: [String]
    let states: [String]
    let orders: [String]
}

private struct CanonicalParseOptions: Codable {
    let smartPunctuation: Bool
    let footnotes: Bool
    let stripHTMLComments: Bool
    let tables: Bool
    let strikethrough: Bool
    let autolinks: Bool
    let taskLists: Bool
    let formulas: Bool
    let dollarFormulaDelimiters: Bool
    let latexFormulaDelimiters: Bool
    let directives: Bool

    private enum CodingKeys: String, CodingKey, CaseIterable {
        case smartPunctuation
        case footnotes
        case stripHTMLComments
        case tables
        case strikethrough
        case autolinks
        case taskLists
        case formulas
        case dollarFormulaDelimiters
        case latexFormulaDelimiters
        case directives
    }

    init(from decoder: Decoder) throws {
        let dynamic = try decoder.container(keyedBy: DynamicCodingKey.self)
        let actual = Set(dynamic.allKeys.map(\.stringValue))
        let expected = Set(CodingKeys.allCases.map(\.rawValue))
        guard actual == expected else {
            let missing = expected.subtracting(actual).sorted().joined(separator: ", ")
            let unknown = actual.subtracting(expected).sorted().joined(separator: ", ")
            throw GeneratorFailure.invalidManifest(
                "parseOptions keys differ; missing=[\(missing)] unknown=[\(unknown)]"
            )
        }

        let values = try decoder.container(keyedBy: CodingKeys.self)
        smartPunctuation = try values.decode(Bool.self, forKey: .smartPunctuation)
        footnotes = try values.decode(Bool.self, forKey: .footnotes)
        stripHTMLComments = try values.decode(Bool.self, forKey: .stripHTMLComments)
        tables = try values.decode(Bool.self, forKey: .tables)
        strikethrough = try values.decode(Bool.self, forKey: .strikethrough)
        autolinks = try values.decode(Bool.self, forKey: .autolinks)
        taskLists = try values.decode(Bool.self, forKey: .taskLists)
        formulas = try values.decode(Bool.self, forKey: .formulas)
        dollarFormulaDelimiters = try values.decode(Bool.self, forKey: .dollarFormulaDelimiters)
        latexFormulaDelimiters = try values.decode(Bool.self, forKey: .latexFormulaDelimiters)
        directives = try values.decode(Bool.self, forKey: .directives)
    }
}

private struct GeneratedManifest: Encodable {
    let schemaVersion: Int
    let cases: [GeneratedCase]
}

private struct GeneratedCase: Encodable {
    let name: String
    let source: String
    let expected: String
    let parseOptions: CanonicalParseOptions
}

private struct DynamicCodingKey: CodingKey {
    let stringValue: String
    let intValue: Int?

    init?(stringValue: String) {
        self.stringValue = stringValue
        intValue = nil
    }

    init?(intValue: Int) {
        stringValue = String(intValue)
        self.intValue = intValue
    }
}
